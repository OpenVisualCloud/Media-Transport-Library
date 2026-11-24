# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
import requests
from compliance import compliance_client
from compliance.compliance_client import PcapComplianceClient
from create_pcap_file.netsniff import NetsniffRecorder
from mtl_engine.application_base import Application
from mtl_engine.integrity_session import NO_INTEGRITY
from mtl_engine.pcap_compliance import CaptureIntent
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


def test_rx_timing_without_narrow_still_needs_every_result():
    """Skipping the narrow verdict keeps the check that every session reported."""
    app = _rxtxapp({})
    app.last_output = "rv_tp_stat(0,0), COMPLIANT NARROW 0 WIDE 4 FAILED 0"

    app.assert_rx_timing_compliance(expected_sessions=1, require_narrow=False)
    with pytest.raises(AssertionError, match=r"2 were expected"):
        app.assert_rx_timing_compliance(expected_sessions=2, require_narrow=False)


def test_rx_timing_counts_frame_level_legacy_video_only():
    """The parser runs on a frame-level RX video session, never an RTP-level one."""
    app = _rxtxapp(
        {
            "rx_sessions": [
                {
                    "interface": [1],
                    "video": [{"replicas": 1, "type": "frame"}],
                },
                {
                    "interface": [1],
                    "video": [{"replicas": 1, "type": "rtp"}],
                },
            ],
        }
    )

    assert app._expected_rx_timing_results() == 1


def test_rx_first_packet_times_keep_a_period_with_no_frame():
    """A no-frame period prints FPT -1; it must reach the test, not vanish."""
    app = _rxtxapp({})
    app.last_output = "\n".join(
        [
            "rv_tp_stat(0), FPT AVG 625693.06 MIN 619520 MAX 630528 DIFF 11008!",
            "rv_tp_stat(0), FPT AVG -1.00 MIN 0 MAX 0 DIFF 0!",
        ]
    )

    assert app.rx_first_packet_times_ns() == [625693.06, -1.0]


def test_tx_user_timestamp_errors_sum_the_periods_and_need_a_stat_dump():
    """Per-period rises add up; no stat dump at all is -1, not a silent 0."""
    app = _rxtxapp({})
    app.last_output = "app_tx_video_result(0), OK"
    assert app.count_tx_user_timestamp_errors() == -1

    app.last_output = "TX_VIDEO_SESSION(0,0): throughput 2.6 Mb/s"
    assert app.count_tx_user_timestamp_errors() == 0

    app.last_output = "\n".join(
        [
            "TX_VIDEO_SESSION(0,0): error user timestamp 3",
            "TX_VIDEO_SESSION(0,0): error user timestamp 2",
        ]
    )
    assert app.count_tx_user_timestamp_errors() == 5


def test_tx_user_pacing_applied_reads_the_rxtxapp_line():
    """Only RxTxApp's "user_pacing on" line counts, its "ignored" warning does not."""
    app = _rxtxapp({})
    app.last_output = "app_tx_video_init(0), user_pacing is rtp level only, ignored"
    assert not app.tx_user_pacing_applied()

    app.last_output = "app_tx_video_init(0), user_pacing on, exact 1"
    assert app.tx_user_pacing_applied()


def test_rejected_needs_both_a_failed_run_and_the_mtl_error():
    app = _rxtxapp({})
    marker = "invalid fmd_dit"
    app.last_output = f"tx_ancillary_ops_check, {marker} 0x300000"

    app.last_return_code = 0
    with pytest.raises(AssertionError, match="accepted a setting"):
        app.assert_rejected(marker)

    app.last_return_code = 1
    app.last_output = "EAL: VFIO group is not viable"
    with pytest.raises(AssertionError, match="failed without"):
        app.assert_rejected(marker)

    app.last_output = f"tx_ancillary_ops_check, {marker} 0x300000"
    app.assert_rejected(marker)


def _per_port_arrivals(*intervals) -> str:
    # The line libmtl prints each stat interval (st_rx_video_session.c), one per
    # (P, R) pair; assert_redundant_paths() reads only the two pkt counts.
    return "\n".join(
        f"RX_VIDEO_SESSION(0,0): per-port arrivals P={p} pkts (0 frames complete, "
        f"0 needed redundancy), R={r} pkts (0 frames complete, 0 needed redundancy)"
        for p, r in intervals
    )


@pytest.mark.parametrize(
    "intervals", [[(1000, 1000)], [(1000, 500)]], ids=["equal", "r_half"]
)
def test_redundant_paths_accept_a_leg_with_half_the_other(intervals):
    app = _rxtxapp({})
    app.last_output = _per_port_arrivals(*intervals)
    app.assert_redundant_paths()


@pytest.mark.parametrize(
    "intervals",
    [
        [(1000, 0)],
        [(1000, 499)],
        [(499, 1000)],
        [(1000, 1000)] + [(1000, 0)] * 3 + [(1000, 1000)],
    ],
    ids=["no_r", "r_short", "p_short", "r_gap"],
)
def test_redundant_paths_reject_a_leg_with_less_than_half(intervals):
    app = _rxtxapp({})
    app.last_output = _per_port_arrivals(*intervals)
    with pytest.raises(AssertionError, match="ST 2022-7 receiver got"):
        app.assert_redundant_paths()


def test_redundant_paths_read_only_the_video_line():
    app = _rxtxapp({})
    app.last_output = (
        "RX_AUDIO_SESSION(0,0): per-port arrivals P=1000 pkts (0 frames first), "
        "R=1000 pkts (0 frames first)"
    )
    with pytest.raises(AssertionError, match="no per-port arrival counts"):
        app.assert_redundant_paths()


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


class _MinimalApp(Application):
    """Concrete stub so create_command()'s dma_dev precedence can be tested
    without RxTxApp's session-type-specific config building getting involved.
    """

    def get_app_name(self):
        return "minimal"

    def get_executable_name(self):
        return "minimal"

    def _create_command_and_config(self):
        return "minimal", None

    def validate_results(self, fail_on_error: bool = True) -> bool:
        return True


def test_create_command_defaults_dma_auto_from_app_factory_when_unset():
    """app_factory stashes the host's bound DMA channel(s) on
    _default_dma_dev; create_command() must turn on dma_auto when the test
    itself never mentions dma_dev.
    """
    app = _MinimalApp(app_path="minimal")
    app._default_dma_dev = "0000:80:01.0,0000:85:01.0"

    app.create_command(session_type="st20p")

    assert app.params["dma_auto"] is True
    assert app.params["dma_dev"] is None


def test_create_command_explicit_dma_dev_overrides_default():
    """A test that passes dma_dev= itself must win over app_factory's
    default -- the precedence app_factory's docstring promises.
    """
    app = _MinimalApp(app_path="minimal")
    app._default_dma_dev = "0000:80:01.0"

    app.create_command(session_type="st20p", dma_dev="0000:aa:01.0")

    assert app.params["dma_dev"] == "0000:aa:01.0"
    assert app.params["dma_auto"] is False


def test_create_command_keeps_dma_auto_off_for_a_tx_only_process():
    """A TX-only process must not probe the DMA devices: vfio-pci gives a
    device to one process, and the RX of the same test needs it.
    """
    app = _MinimalApp(app_path="minimal")
    app._default_dma_dev = "0000:80:01.0"

    app.create_command(session_type="st20p", direction="tx")

    assert app.params["dma_auto"] is False


def test_create_command_leaves_dma_dev_none_without_a_default():
    """A host whose host_dma_devices found no DMA channel to bind must behave
    exactly as before this feature existed.
    """
    app = _MinimalApp(app_path="minimal")

    app.create_command(session_type="st20p")

    assert app.params["dma_dev"] is None
    assert app.params["dma_auto"] is False


@pytest.mark.parametrize(
    "capture_filter, expected",
    [
        ("dst 239.1.0.1", ["239.1.0.1"]),
        ("(dst 239.1.0.1 or dst 239.1.0.2)", ["239.1.0.1", "239.1.0.2"]),
        ("src 192.168.0.8 and dst 239.1.0.1", ["239.1.0.1"]),
        ("dst 192.168.17.2", []),
        (None, []),
    ],
)
def test_netsniff_drops_only_multicast_capture_destinations(capture_filter, expected):
    recorder = object.__new__(NetsniffRecorder)
    recorder.capture_filter = capture_filter
    assert recorder._multicast_dst_ips() == expected


class _NetdevConn:
    """A host connection that records commands and keeps the PROMISC flag."""

    def __init__(self, process=None):
        self.promisc, self.cmds, self.process = False, [], process

    def execute_command(self, cmd, **_):
        self.cmds.append(cmd)
        if "promisc on" in cmd:
            self.promisc = True
        flags = "<BROADCAST,MULTICAST,PROMISC,UP>" if self.promisc else "<UP>"
        return type("Res", (), {"return_code": 0, "stdout": flags, "stderr": ""})

    def start_process(self, cmd, **_):
        return self.process


def test_netsniff_restores_promisc_after_a_second_start():
    recorder = object.__new__(NetsniffRecorder)
    recorder.interface, recorder._promisc_was_off = "eth0", False
    recorder.host = type("Host", (), {"connection": _NetdevConn()})
    recorder._enable_promisc(recorder.host.connection)
    recorder._enable_promisc(recorder.host.connection)
    recorder._restore_promisc()
    assert recorder.host.connection.cmds[-1] == "sudo ip link set dev eth0 promisc off"


def test_netsniff_counted_capture_restores_the_netdev_when_it_ends(monkeypatch):
    monkeypatch.setattr("create_pcap_file.netsniff.sleep", lambda _: None)

    class Proc:
        running, pid, stdout_text = True, 1, ""

        def wait(self, timeout):
            self.running = False

    conn = _NetdevConn(Proc())
    recorder = NetsniffRecorder(
        type("Host", (), {"connection": conn, "name": "h"}),
        "t",
        "/tmp",
        interface="eth0",
        capture_filter="dst 239.1.0.1",
        packets_capture=10,
    )
    recorder.capture()
    drop_del = "sudo tc filter del dev eth0 ingress pref 2110"
    assert conn.cmds.count(drop_del) == 2  # before the capture and after it
    # The rule goes in before promisc and comes out after it: a promiscuous PF
    # without the drop rule is the worst state.
    add = next(i for i, c in enumerate(conn.cmds) if "tc filter add dev eth0" in c)
    assert add < conn.cmds.index("sudo ip link set dev eth0 promisc on")
    undo = len(conn.cmds) - 1 - conn.cmds[::-1].index(drop_del)
    assert "sudo ip link set dev eth0 promisc off" in conn.cmds[:undo]
    assert recorder.pcap_file  # evaluate() still uploads it
    seen = len(conn.cmds)
    recorder.stop()  # ComplianceSession.close() only reaps again
    assert all("pkill" in c for c in conn.cmds[seen:])


class _FakeClock:
    """Time that only moves when the code under test sleeps."""

    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class _FakeResponse:
    def __init__(self, status_code, report=None):
        self.status_code = status_code
        self._report = report

    def raise_for_status(self):
        if self.status_code >= 400:
            raise requests.HTTPError(f"HTTP {self.status_code}")

    def json(self):
        return self._report


class _FakeEbuList:
    """A LIST report endpoint that answers 404, then not-analyzed, then the report
    once *ready_after* seconds have passed; *errors_first* calls raise first."""

    def __init__(self, clock, ready_after, errors_first=0):
        self.clock = clock
        self.ready_after = ready_after
        self.errors_first = errors_first
        self.calls = 0

    def get(self, url, **kwargs):
        assert 0 < kwargs["timeout"] <= 10  # a hung LIST cannot outlast the wait
        self.calls += 1
        if self.calls <= self.errors_first:
            raise requests.ConnectionError("connection reset")
        if self.clock.now < self.ready_after / 2:
            return _FakeResponse(404)
        if self.clock.now < self.ready_after:
            return _FakeResponse(200, {"analyzed": False})
        return _FakeResponse(200, {"analyzed": True, "streams": [{}]})


def _report_client(monkeypatch, ebu_list, clock):
    monkeypatch.setattr(compliance_client.time, "monotonic", clock.monotonic)
    monkeypatch.setattr(compliance_client.time, "sleep", clock.sleep)
    client = object.__new__(PcapComplianceClient)
    client.ebu_ip = "list"
    client.pcap_id = "uuid"
    client.token = "t"
    client.proxies = {}
    client.session = ebu_list
    return client


def test_ebu_report_waits_out_a_backlogged_analyser(monkeypatch):
    clock = _FakeClock()
    client = _report_client(monkeypatch, _FakeEbuList(clock, ready_after=30), clock)

    report = client.download_report()

    assert report and report["analyzed"]


def test_ebu_report_gives_up_on_an_analyser_that_never_answers(monkeypatch):
    clock = _FakeClock()
    ebu_list = _FakeEbuList(clock, ready_after=float("inf"))
    client = _report_client(monkeypatch, ebu_list, clock)

    report = client.download_report()

    assert report is False
    assert compliance_client.REPORT_WAIT_S - 10 <= clock.now
    assert clock.now <= compliance_client.REPORT_WAIT_S
    assert ebu_list.calls < 30  # backs off instead of polling every second


def test_ebu_report_survives_a_dropped_connection(monkeypatch):
    clock = _FakeClock()
    client = _report_client(monkeypatch, _FakeEbuList(clock, 2, errors_first=2), clock)

    report = client.download_report()

    assert report and report["analyzed"]
