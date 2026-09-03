# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from compliance.compliance_client import PcapComplianceClient, no_verdict_reason
from mtl_engine import pcap_compliance
from mtl_engine.application_base import Application
from mtl_engine.integrity_session import NO_INTEGRITY
from mtl_engine.pcap_compliance import CaptureIntent, ComplianceSession
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


# What EBU LIST returns for a capture that recorded nothing.
EMPTY_CAPTURE_REPORT = {
    "analyzed": True,
    "error": "",
    "total_streams": 0,
    "video_streams": 0,
    "not_compliant_streams": 0,
    "streams": [],
}


def test_ebu_report_states_are_distinct(caplog):
    client = object.__new__(PcapComplianceClient)
    unavailable, _ = client.check_compliance(False)
    no_streams, _ = client.check_compliance(EMPTY_CAPTURE_REPORT)
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
    assert no_streams is None, "no streams is no verdict, not a non-compliance"
    assert non_compliant is False
    assert compliant is True

    reason = no_verdict_reason(EMPTY_CAPTURE_REPORT)
    assert "no ST 2110 streams" in reason
    assert reason != no_verdict_reason(False), "unavailable and empty differ"
    # A report that omits the key entirely, not just one with an empty list.
    assert no_verdict_reason({"analyzed": True, "total_streams": 0}) == reason
    # The site that first detects the fault must log which one it found.
    assert reason in caplog.messages
    assert no_verdict_reason(False) in caplog.messages


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


def test_the_verdict_step_names_an_empty_capture_as_the_cause(monkeypatch):
    """The AssertionError a test actually sees must name the real fault."""

    class FakeUploaded:
        return_code = 0
        stdout = ">>>UUID: 0e5b5089-0000-0000-0000-000000000000\n"
        stderr = ""

    class FakeConnection:
        def execute_command(self, *args, **kwargs):
            return FakeUploaded()

    class FakeHost:
        connection = FakeConnection()

    class FakeRecorder:
        pcap_file = "/mnt/ramdisk/pcap/empty.pcap"
        host = FakeHost()

    class FakeUploader:
        """The real verdict logic over a canned report, with no network."""

        def download_report(self):
            return EMPTY_CAPTURE_REPORT

        check_compliance = PcapComplianceClient.check_compliance

    recorded = []
    monkeypatch.setattr(
        pcap_compliance, "PcapComplianceClient", lambda **kw: FakeUploader()
    )
    monkeypatch.setattr(
        pcap_compliance,
        "update_compliance_result",
        lambda node_id, result: recorded.append((node_id, result)),
    )

    session = object.__new__(ComplianceSession)
    session._recorder = FakeRecorder()
    session.ebu_server = {"ebu_ip": "127.0.0.1", "user": "u", "password": "p"}
    session.mtl_path = "/repo"
    session.node_id = "tests/single/st20p/test_fps.py::test_st20p_fps"

    with pytest.raises(AssertionError) as error:
        session._fetch_report(fail_on_error=False)

    message = str(error.value)
    assert "no ST 2110 streams" in message
    assert "reported non-compliance" not in message
    # conftest.py matches this string exactly; another spelling records a Pass.
    assert recorded == [(session.node_id, "Fail")]
