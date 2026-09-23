# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import logging
from datetime import datetime, timedelta

import pytest
from compliance.compliance_client import PcapComplianceClient
from mtl_engine.application_base import Application
from mtl_engine.integrity_session import NO_INTEGRITY
from mtl_engine.pcap_compliance import CaptureIntent
from mtl_engine.performance_monitoring import (
    FPS_TOLERANCE_PCT,
    MAX_DROP_PCT,
    MTL_STAT_INTERVAL_S,
    _walk_dumps,
    companion_steady_window,
    monitor_dev_rate,
    monitor_rx_fps,
    monitor_tx_fps,
    paced_fps,
)
from mtl_engine.rxtxapp import RxTxApp


def _rxtxapp(config: dict) -> RxTxApp:
    app = object.__new__(RxTxApp)
    app.config = config
    return app


def test_rxtxapp_counts_replicas_and_redundant_paths():
    app = _rxtxapp(
        {
            "tx_sessions": [
                {
                    "dip": ["239.1.0.1", "239.1.0.2"],
                    "interface": [0, 1],
                    "st20p": [{"replicas": 2}],
                }
            ],
            "rx_sessions": [
                {
                    "interface": [2, 3],
                    "st20p": [{"replicas": 2}],
                }
            ],
        }
    )

    assert app._expected_video_streams() == 4
    assert app._expected_rx_timing_results() == 4


def test_rx_timing_requires_every_result_to_be_narrow():
    app = _rxtxapp({})
    app.last_output = "\n".join(
        [
            "rv_tp_stat(0,0), COMPLIANT NARROW 4 WIDE 0 FAILED 0",
            "rv_tp_stat(1,0), COMPLIANT NARROW 0 WIDE 0 FAILED 4",
        ]
    )

    with pytest.raises(AssertionError, match="did not report narrow compliance"):
        app.assert_rx_timing_compliance(expected_sessions=2)

    with pytest.raises(AssertionError, match=r"3 were expected"):
        app.assert_rx_timing_compliance(expected_sessions=3)


def test_ebu_report_states_are_distinct():
    client = object.__new__(PcapComplianceClient)
    unavailable, _ = client.check_compliance(False)
    non_compliant, _ = client.check_compliance(
        {
            "analyzed": True,
            "not_compliant_streams": 1,
            "streams": [{"media_type": "video"}],
        }
    )
    compliant, _ = client.check_compliance(
        {
            "analyzed": True,
            "not_compliant_streams": 0,
            "streams": [{"media_type": "video"}],
        }
    )

    assert unavailable is None
    assert non_compliant is False
    assert compliant is True


def test_finalize_reports_both_ebu_and_mtl_failures():
    class FailingCompliance:
        def evaluate(self, intent, fail_on_error):
            raise AssertionError("EBU analyzed non-compliant")

    class FailingApplication:
        def _dispatch_validate(self, fail_on_error):
            raise AssertionError("MTL parser reported failed frames")

    intent = CaptureIntent(dst_ips=("239.1.0.1",), capture_time=1)
    with pytest.raises(AssertionError) as error:
        Application._finalize_run(
            FailingApplication(),
            FailingCompliance(),
            intent,
            True,
            integrity=NO_INTEGRITY,
        )

    assert "EBU analyzed non-compliant" in str(error.value)
    assert "MTL parser reported failed frames" in str(error.value)


# ── Performance sweep: rate and FPS oracles ─────────────────────────────

# One 1080p59.94 ST2110-20 stream (YUV 4:2:2 10-bit, GPM, 1500 B MTU) costs
# 3928 packets and 5,433,422 L2 bytes per frame, plus the 24 bytes per packet
# the wire spends on FCS, preamble/SFD and the inter-frame gap.  Derived and
# checked against measured DEV rates in doc/perf_capacity.md §4.
SESSION_L2_MBPS = 2605.44
SESSION_L1_MBPS = 2650.64
SESSION_PKTS_PER_FRAME = 3928


def _pkts_per_dump(fps):
    """Packets one 1080p session puts on the wire in a stat period at *fps*."""
    return int(SESSION_PKTS_PER_FRAME * fps * MTL_STAT_INTERVAL_S)


def _tx_sweep_dumps(fps):
    """Ten TX stat dumps spanning one sweep iteration of two sessions.

    Each entry is ``(timestamp, [fps per session named], DEV tx Mb/s)``, shaped
    like a real iteration: one ramp-up dump whose period covers session create,
    six dumps of steady traffic at *fps*, then teardown — a short census over a
    partial period while ``st20p_tx_free()`` works through the sessions, then an
    idle port.  The six steady rates average to exactly the demand at *fps* —
    a session's byte rate is linear in its frame rate, so a fixture running slow
    has to carry proportionally fewer bytes.
    """
    l2 = 2 * SESSION_L2_MBPS * fps / paced_fps(59)
    return [
        ("2026-09-23 00:43:56", [42.30, 59.38], 0.46 * l2),
        ("2026-09-23 00:44:06", [fps, fps], l2 - 3.0),
        ("2026-09-23 00:44:16", [fps, fps], l2 - 2.0),
        ("2026-09-23 00:44:26", [fps, fps], l2 - 1.0),
        ("2026-09-23 00:44:36", [fps, fps], l2 + 1.0),
        ("2026-09-23 00:44:46", [fps, fps], l2 + 2.0),
        ("2026-09-23 00:44:56", [fps, fps], l2 + 3.0),
        ("2026-09-23 00:45:04", [38.50], 0.3 * l2),
        ("2026-09-23 00:45:14", [0.0], 0.0),
        ("2026-09-23 00:45:24", [0.0], 0.0),
    ]


def _sweep_log(dumps, direction="TX", ports=1, late_census=()):
    """Render *dumps* as the log lines the dual-host test parses → list[str].

    Each dump is wrapped in the banners ``stat_dump()`` prints around it
    (``lib/src/mt_stat.c``), and carries one ``DEV`` line per port and one
    ``{TX,RX}_VIDEO_SESSION`` line per session named, in the shape
    ``dev_inf_stat()`` and the session stat functions emit, behind the
    ``TX:``/``RX:`` prefix the test's log relay adds.

    MTL stamps each line as it prints it, so a dump can straddle a second.
    *late_census* names the dumps to render the way every such dump in the
    baseline run was: banner and ``DEV`` lines on one second, the session census
    and closing banner on the next.
    """
    tx = direction == "TX"
    lines = []
    for i, (ts, session_fps, rate) in enumerate(dumps):
        # Packets follow the frame rate, as the byte rate does: a session running
        # slow carries proportionally fewer of both.
        pkts = sum(_pkts_per_dump(f) for f in session_fps)
        rates = (rate, 0.0) if tx else (0.0, rate)
        counts = (pkts, 0) if tx else (0, pkts)
        census_ts = ts
        if i in late_census:
            base = datetime.strptime(ts, "%Y-%m-%d %H:%M:%S")
            census_ts = f"{base + timedelta(seconds=1):%Y-%m-%d %H:%M:%S}"
        lines.append(f"{direction}: MTL: {ts}, * *    M T    D E V   S T A T E   * * ")
        for port in range(ports):
            lines.append(
                f"{direction}: MTL: {ts}, DEV({port}): Avr rate, tx: {rates[0]:f} "
                f"Mb/s, rx: {rates[1]:f} Mb/s, pkts, tx: {counts[0]}, rx: {counts[1]}"
            )
        for sid, fps in enumerate(session_fps):
            app = "app_tx_st20p" if tx else "app_rx_st20p"
            lines.append(
                f"{direction}: MTL: {census_ts}, "
                f"{direction}_VIDEO_SESSION(1,{sid}:{app}_{sid}): "
                f"fps {fps:f} frames {int(fps * MTL_STAT_INTERVAL_S)} "
                f"pkts {_pkts_per_dump(fps)}:0 inflight 0:0"
            )
        lines.append(f"{direction}: MTL: {census_ts}, * *    E N D    S T A T E   * * ")
    return lines


def _all_dumps(lines):
    """Every stat dump in *lines* as a window — what a fixed wall-clock warmup left."""
    return list(dict.fromkeys(dump for dump, _ in _walk_dumps(lines)))


def _measured_window(lines):
    """The window the measured host's rate is taken over, as production derives it.

    The measured side takes its window from the FPS verdict, which unlike
    :func:`companion_steady_window` does not trim an idle tail, so take it the
    way the dual-host test does.
    """
    _, _, details = monitor_tx_fps(lines, paced_fps(59), 2, max_drop_pct=MAX_DROP_PCT)
    return details["window"]


def test_rate_metrics_exclude_ramp_up_and_teardown_dumps():
    """Teardown dumps must not dilute the port rate a sweep publishes.

    ``st20p_tx_free()`` costs roughly a second per session, so a TX app keeps
    emitting 10 s stat dumps well after traffic stops: one partial period, then
    zeros (doc/perf_capacity.md §6).
    """
    dumps = _tx_sweep_dumps(paced_fps(59))
    lines = _sweep_log(dumps)

    window = _measured_window(lines)
    assert [dump.ts for dump in window] == [
        "2026-09-23 00:44:16",
        "2026-09-23 00:44:26",
        "2026-09-23 00:44:36",
        "2026-09-23 00:44:46",
    ]

    rates = monitor_dev_rate(lines, window)["tx_rate_history"]
    assert len(rates) == len(window)
    assert sum(rates) / len(rates) == pytest.approx(2 * SESSION_L2_MBPS)

    # Averaging every dump instead — what a fixed wall-clock warmup did — loses
    # a third of the rate.  This is the defect, reproduced.
    every = monitor_dev_rate(lines, _all_dumps(lines))["tx_rate_history"]
    assert sum(every) / len(every) < 0.75 * 2 * SESSION_L2_MBPS


def test_a_dump_straddling_a_second_still_reaches_the_rate():
    """A dump whose census is stamped a second after its DEV line still counts.

    MTL stamps each stat line as it prints it rather than once per dump, so a
    dump can put its ``DEV`` line on the banner's second and every session line
    on the next.  Matched by stamp, the port line of such a dump is lost from a
    window the census defines (doc/perf_capacity.md §6.2).
    """
    dumps = _tx_sweep_dumps(paced_fps(59))
    lines = _sweep_log(dumps, late_census=(2, 4, 5))

    window = _measured_window(lines)
    rates = monitor_dev_rate(lines, window)["tx_rate_history"]

    assert len(rates) == len(window)
    assert sum(rates) / len(rates) == pytest.approx(2 * SESSION_L2_MBPS)


def test_stats_printed_as_sessions_are_freed_are_not_a_dump():
    """The last dump naming every session is dropped, even when the app frees fast.

    Freeing a session prints its stats once more, outside any dump
    (``tv_detach()``/``rv_detach()``).  An RX app frees every session within a
    second, so keyed on stamps those lines read as one more dump naming every
    session.  That pseudo-dump then became the last full dump, the one
    :func:`_steady_window` drops, and the real dump the rule means to drop was
    judged instead (doc/perf_capacity.md §6.2).
    """
    fps = paced_fps(59)
    l2 = 2 * SESSION_L2_MBPS
    live = [fps, fps]
    dumps = [
        ("2026-09-23 01:28:21", [0.0, 0.0], 0.0),  # created, sender not up yet
        ("2026-09-23 01:28:31", [32.7, 32.7], 0.55 * l2),  # sender ramping
        ("2026-09-23 01:28:41", live, l2),
        ("2026-09-23 01:28:51", live, l2),
        ("2026-09-23 01:29:01", live, l2),
        ("2026-09-23 01:29:11", live, l2),
        ("2026-09-23 01:29:21", live, l2),
        ("2026-09-23 01:29:31", live, l2),  # the last dump before the app frees
    ]
    # Two seconds on, the app frees both sessions, as in that run's logs.
    lines = _sweep_log(dumps, "RX") + [
        f"RX: MTL: 2026-09-23 01:29:33, RX_VIDEO_SESSION(1,{sid}:app_rx_st20p_{sid}): "
        f"fps 59.175589 frames 139 pkts 545904"
        for sid in (0, 1)
    ]

    _, count, details = monitor_rx_fps(lines, fps, 2, max_drop_pct=MAX_DROP_PCT)
    assert [dump.ts for dump in details["window"]] == [
        "2026-09-23 01:28:41",
        "2026-09-23 01:28:51",
        "2026-09-23 01:29:01",
        "2026-09-23 01:29:11",
        "2026-09-23 01:29:21",
    ]
    assert count == 2


def test_a_redundant_dump_straddling_a_second_reads_as_one_dump(caplog):
    """Two ports, one straddling dump — still one covered dump.

    A redundant config emits a ``DEV`` line per port, so the sample count is a
    multiple of the window and says nothing about which dumps were covered
    (doc/perf_capacity.md §6.2).
    """
    lines = _sweep_log(_tx_sweep_dumps(paced_fps(59)), ports=2, late_census=range(10))

    window = _measured_window(lines)
    with caplog.at_level(logging.WARNING, logger="mtl_engine.performance_monitoring"):
        rates = monitor_dev_rate(lines, window)["tx_rate_history"]

    assert len(window) == 4
    assert len(rates) == 2 * len(window)  # one sample per port per dump
    assert sum(rates) / len(rates) == pytest.approx(2 * SESSION_L2_MBPS)
    # Assert on caplog.messages, not caplog.text -- this suite's log_format
    # carries no %(message)s, so caplog.text renders every record without it.
    assert not [m for m in caplog.messages if "steady-window" in m]


def test_a_three_dump_window_is_refused_rather_than_judged():
    """Four steady dumps is the floor, and the number is asserted, not derived.

    Too few samples is exactly the state in which one partial dump decides the
    verdict, so a run that cannot show four steady dumps is reported as a failure
    with its reason rather than judged on what is left.  The count is spelled out
    here on purpose: deriving it from ``FPS_MIN_STEADY_SAMPLES`` would make this
    test agree with any floor, including one low enough to publish a number that
    two dumps decided.
    """
    fps = paced_fps(59)
    short = _tx_sweep_dumps(fps)
    del short[6]  # one steady dump fewer, so the window is three
    lines = _sweep_log(short)

    _, count, details = monitor_tx_fps(lines, fps, 2, max_drop_pct=MAX_DROP_PCT)
    assert details["window"] == []
    assert "need 4" in details["window_reject_reason"]
    assert count == 0
    assert companion_steady_window(lines, 2, "TX") == []

    # One more steady dump and the very same run is judged.
    _, count, details = monitor_tx_fps(
        _sweep_log(_tx_sweep_dumps(fps)), fps, 2, max_drop_pct=MAX_DROP_PCT
    )
    assert len(details["window"]) == 4
    assert count == 2


def test_companion_window_stops_where_the_far_end_stopped_sending():
    """A killed companion names every session forever, so trim its idle tail.

    The companion app is killed rather than stopped: it frees no session, so the
    census bound has nothing to trim and the window ran on through the dumps
    after the far end stopped (doc/perf_capacity.md §6.1).
    """
    l2 = 2 * SESSION_L2_MBPS
    live = [paced_fps(59), paced_fps(59)]
    dumps = [
        ("2026-09-23 09:51:26", [0.0, 0.0], 0.0),  # created, sender not up yet
        ("2026-09-23 09:51:36", [32.7, 32.7], 0.35 * l2),  # sender ramping
        ("2026-09-23 09:51:46", live, l2 - 1.0),
        ("2026-09-23 09:51:56", live, l2 + 1.0),
        ("2026-09-23 09:52:06", live, l2 - 1.0),
        ("2026-09-23 09:52:16", live, l2 + 1.0),
        ("2026-09-23 09:52:26", [45.0, 45.0], 0.75 * l2),  # sender stopped mid-period
        ("2026-09-23 09:52:36", [0.0, 0.0], 0.0),
        ("2026-09-23 09:52:46", [0.0, 0.0], 0.0),
        ("2026-09-23 09:52:56", [0.0, 0.0], 0.0),
    ]
    lines = _sweep_log(dumps, "RX")

    window = companion_steady_window(lines, 2, "RX")
    assert [dump.ts for dump in window] == [
        "2026-09-23 09:51:46",
        "2026-09-23 09:51:56",
        "2026-09-23 09:52:06",
        "2026-09-23 09:52:16",
    ]
    rates = monitor_dev_rate(lines, window)["rx_rate_history"]
    assert sum(rates) / len(rates) == pytest.approx(l2)


def test_companion_window_keeps_a_log_that_ends_mid_traffic():
    """Only a *trailing* dead run is trimmed, so a mid-window blip still counts.

    A run that collapses to zero and recovers is the failure the rate metric
    exists to show, and a log whose last dump still carries traffic has no idle
    tail to remove.
    """
    l2 = 2 * SESSION_L2_MBPS
    live = [paced_fps(59), paced_fps(59)]
    lines = _sweep_log(
        [
            ("2026-09-23 09:51:26", [0.0, 0.0], 0.0),
            ("2026-09-23 09:51:36", live, l2),
            ("2026-09-23 09:51:46", live, l2),
            ("2026-09-23 09:51:56", [0.0, 0.0], 0.0),  # collapse, mid-window
            ("2026-09-23 09:52:06", live, l2),
            ("2026-09-23 09:52:16", live, l2),
            ("2026-09-23 09:52:26", live, l2),
            ("2026-09-23 09:52:36", live, l2),
        ],
        "RX",
    )

    window = companion_steady_window(lines, 2, "RX")
    rates = monitor_dev_rate(lines, window)["rx_rate_history"]
    assert len(rates) == 5  # the collapse is kept; no idle tail, so no trim
    assert 0.0 in rates
    assert sum(rates) / len(rates) == pytest.approx(0.8 * l2)


def test_dev_rate_reports_the_wire_rate_beside_the_l2_counter():
    """The wire (L1) rate is the only one comparable against a port's speed.

    The DPDK octet counters exclude FCS, preamble and the inter-frame gap —
    1.7 % here, the gap that made a 94 Gb/s reading on a 100 G port look like
    room for more sessions.
    """
    lines = _sweep_log(_tx_sweep_dumps(paced_fps(59)))
    window = _measured_window(lines)
    wire = monitor_dev_rate(lines, window)["tx_wire_rate_history"]

    assert sum(wire) / len(wire) == pytest.approx(2 * SESSION_L1_MBPS, rel=1e-4)


def test_fps_bar_is_the_paced_rate_not_the_format_token():
    """`p59` is 60000/1001, so 58.50 fps is dropping frames, not a pass.

    Scoring against the integer token 59 makes the intended 99 % tolerance
    58.41/59.94 = 97.45 % (doc/perf_capacity.md §7).
    """
    assert paced_fps(59) == pytest.approx(60000 / 1001)
    assert paced_fps(29) == pytest.approx(30000 / 1001)

    lines = _sweep_log(_tx_sweep_dumps(58.50))

    _, count, details = monitor_tx_fps(
        lines, paced_fps(59), 2, max_drop_pct=MAX_DROP_PCT
    )
    assert count == 0
    assert details["min_required_fps"] == pytest.approx(
        paced_fps(59) * FPS_TOLERANCE_PCT
    )

    # The token used as a target is what let those sessions through.
    _, token_count, _ = monitor_tx_fps(lines, 59, 2, max_drop_pct=MAX_DROP_PCT)
    assert token_count == 2


def test_trimmed_mean_discards_a_quarter_of_a_minimum_window():
    """Pin how MAX_DROP_PCT and FPS_MIN_STEADY_SAMPLES interact, because it bites.

    ``n_drop = max(1, int(len(hist) * max_drop_pct))`` always drops at least one
    sample, so at the 4-dump floor a 10 % trim removes 25 % of the evidence — and
    the sample it removes is the worst one.  A session that transported nothing
    for one whole 10 s period therefore still passes, on three samples.

    That is a wider tolerance than 10 % anywhere below 10 dumps.  It is left as
    is (doc/perf_capacity.md §10) and pinned so the next person to widen
    MAX_DROP_PCT sees the floor interaction first.
    """
    fps = paced_fps(59)
    dumps = _tx_sweep_dumps(fps)
    # Collapse one session for one whole period inside the 4-dump window.
    ts, session_fps, rate = dumps[3]
    dumps[3] = (ts, [0.0, session_fps[1]], rate / 2)
    lines = _sweep_log(dumps)

    assert len(_measured_window(lines)) == 4  # the floor, exactly

    # Untrimmed, the collapsed session fails: (0 + 3 × 59.94) / 4 = 44.96.
    _, strict, _ = monitor_tx_fps(lines, fps, 2)
    assert strict == 1
    # Trimmed at the production setting, the zero is discarded and it passes.
    _, trimmed, _ = monitor_tx_fps(lines, fps, 2, max_drop_pct=MAX_DROP_PCT)
    assert trimmed == 2
