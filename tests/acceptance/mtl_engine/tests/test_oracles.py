# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from compliance.compliance_client import PcapComplianceClient
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


def test_create_command_defaults_dma_dev_from_app_factory_when_unset():
    """app_factory stashes the host's bound DMA channel(s) on
    _default_dma_dev; create_command() must apply it when the test itself
    never mentions dma_dev.
    """
    app = _MinimalApp(app_path="minimal")
    app._default_dma_dev = "0000:80:01.0,0000:85:01.0"

    app.create_command(session_type="st20p")

    assert app.params["dma_dev"] == "0000:80:01.0,0000:85:01.0"


def test_create_command_explicit_dma_dev_overrides_default():
    """A test that passes dma_dev= itself must win over app_factory's
    default -- the precedence app_factory's docstring promises.
    """
    app = _MinimalApp(app_path="minimal")
    app._default_dma_dev = "0000:80:01.0"

    app.create_command(session_type="st20p", dma_dev="0000:aa:01.0")

    assert app.params["dma_dev"] == "0000:aa:01.0"


def test_create_command_leaves_dma_dev_none_without_a_default():
    """A host test_config never populated dma_device for (no nightly `dma`
    bind step ran) must behave exactly as before this feature existed.
    """
    app = _MinimalApp(app_path="minimal")

    app.create_command(session_type="st20p")

    assert app.params["dma_dev"] is None
