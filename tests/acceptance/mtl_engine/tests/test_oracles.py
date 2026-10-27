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


def test_netsniff_restores_promisc_after_a_second_start():
    class Conn:
        def __init__(self):
            self.promisc, self.cmds = False, []

        def execute_command(self, cmd, **_):
            self.cmds.append(cmd)
            if "promisc on" in cmd:
                self.promisc = True
            flags = "<BROADCAST,MULTICAST,PROMISC,UP>" if self.promisc else "<UP>"
            return type("Res", (), {"return_code": 0, "stdout": flags, "stderr": ""})

    recorder = object.__new__(NetsniffRecorder)
    recorder.interface, recorder._promisc_was_off = "eth0", False
    recorder.host = type("Host", (), {"connection": Conn()})
    recorder._enable_promisc(recorder.host.connection)
    recorder._enable_promisc(recorder.host.connection)
    recorder._restore_promisc()
    assert recorder.host.connection.cmds[-1] == "sudo ip link set dev eth0 promisc off"


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
