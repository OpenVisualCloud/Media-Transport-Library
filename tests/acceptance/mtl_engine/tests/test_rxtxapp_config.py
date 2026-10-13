# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from mtl_engine import ip_pools
from mtl_engine.rxtxapp import RxTxApp

PAYLOAD = "/mnt/ramdisk/media/payload.txt"


def _config(**params) -> dict:
    if not ip_pools.tx:
        ip_pools.init(1)
    app = RxTxApp(app_path="/unused")
    app.set_params(
        nic_port_list=["0000:00:01.0", "0000:00:01.1"],
        test_mode="unicast",
        **params,
    )
    return app._create_rxtxapp_config_dict()


def test_fast_metadata_is_an_st40p_session_opt_in():
    config = _config(
        session_type="st40p",
        fast_metadata=True,
        fastmetadata_data_item_type=0x3FFFFF,
        fastmetadata_k_bit=1,
        framerate="p50",
        input_file=PAYLOAD,
        payload_type=120,
    )

    tx = config["tx_sessions"][0]["st40p"][0]
    rx = config["rx_sessions"][0]["st40p"][0]
    for session in (tx, rx):
        assert session["fast_metadata"] is True
        assert session["fastmetadata_data_item_type"] == 0x3FFFFF
        assert session["fastmetadata_k_bit"] == 1
        assert session["payload_type"] == 120
        assert session["fps"] == "p50"
        # TX sends the file, RX compares every data item against it.
        assert session["st40p_url"] == PAYLOAD
    assert all("fastmetadata" not in group for group in config["tx_sessions"])
    assert all("fastmetadata" not in group for group in config["rx_sessions"])


def test_fast_metadata_on_an_ancillary_session_is_refused():
    with pytest.raises(ValueError, match="fast_metadata needs session_type st40p"):
        _config(session_type="ancillary", fast_metadata=True, ancillary_url=PAYLOAD)


def test_plain_st40p_session_carries_no_fast_metadata_keys():
    config = _config(session_type="st40p", input_file=PAYLOAD)

    tx = config["tx_sessions"][0]["st40p"][0]
    rx = config["rx_sessions"][0]["st40p"][0]
    for session in (tx, rx):
        assert not any(
            key.startswith(("fast_metadata", "fastmetadata")) for key in session
        )
    assert "st40p_url" not in rx


FMD_RX_OK = (
    "app_rx_st40p_result(0), OK, fps 59.940060, 1797 frames received, "
    "seq_discont 0, marker_missing 0, seq_lost 0, 0 invalid, max gap 16.712 ms"
)


def _fmd_run(output: str, return_code) -> RxTxApp:
    if not ip_pools.tx:
        ip_pools.init(1)
    app = RxTxApp(app_path="/unused")
    app.create_command(
        session_type="st40p",
        fast_metadata=True,
        nic_port_list=["0000:00:01.0", "0000:00:01.1"],
        input_file=PAYLOAD,
        framerate="p59",
        payload_type=115,
    )
    app.last_output = output
    app.last_return_code = return_code
    return app


def test_fast_metadata_rx_ok_result_passes():
    assert _fmd_run(FMD_RX_OK, 0).validate_results(fail_on_error=False)


@pytest.mark.parametrize(
    "output, return_code",
    [
        (FMD_RX_OK.replace(", OK,", ", FAILED,").replace(" 0 inv", " 3 inv"), 0),
        (FMD_RX_OK.replace(", OK,", ", FAILED,"), 251),
        ("app_rx_st40p_result(0), no valid frame, 1797 rejected as invalid", 234),
        ("app_rx_st40p_frame_thread(0), stop", 0),
        ("app_rx_st40p_frame_thread(0), stop", None),
        (
            "tx_ancillary_ops_check, invalid fmd_dit 0x300000, 0x300000-0x3fefff is "
            "reserved\nmain, st_app_tx_st40p_sessions_init fail -5",
            251,
        ),
    ],
    ids=[
        "failed_line",
        "failed_line_rc",
        "no_valid_frame",
        "no_result_line",
        "no_result_line_no_rc",
        "tx_create_refused",
    ],
)
def test_fast_metadata_broken_rx_run_fails(output, return_code):
    with pytest.raises(AssertionError):
        _fmd_run(output, return_code).validate_results(fail_on_error=False)


def test_fast_metadata_rx_result_must_show_the_fast_metadata_check():
    # The result line of an RxTxApp without st40p fast metadata (main before
    # this change): it ignores the fast_metadata keys and runs RFC 8331.
    rfc8331_ok = FMD_RX_OK.split(", 0 invalid")[0]
    with pytest.raises(AssertionError):
        _fmd_run(rfc8331_ok, 0).validate_results(fail_on_error=False)


def test_fast_metadata_rx_result_with_lost_data_items_fails():
    # RX resyncs after a lost item and stays OK while the rate is within 5%,
    # e.g. when the padded last item of each pass over the file is dropped.
    lossy_ok = FMD_RX_OK.replace("seq_discont 0", "seq_discont 27").replace(
        "seq_lost 0", "seq_lost 27"
    )
    with pytest.raises(AssertionError):
        _fmd_run(lossy_ok, 0).validate_results(fail_on_error=False)


def test_fast_metadata_rejection_markers_are_the_lib_errors():
    from pathlib import Path

    from tests.single.st40p import test_fast_metadata as cases

    source = (
        Path(__file__).resolve().parents[4] / "lib/src/st2110/st_tx_ancillary_session.c"
    ).read_text()
    assert f'0x%x, {cases.RESERVED_DIT_ERROR}\\n"' in source
    assert f"%u, {cases.STATIC_PAYLOAD_TYPE_ERROR}\\n" in source


def test_gstreamer_refuses_fast_metadata():
    from mtl_engine.gstreamer import GStreamer

    if not ip_pools.tx:
        ip_pools.init(1)
    with pytest.raises(ValueError, match="fast_metadata"):
        GStreamer().create_command(
            session_type="st40p",
            fast_metadata=True,
            nic_port_list=["0000:00:01.0", "0000:00:01.1"],
            input_file=PAYLOAD,
            framerate="p59",
        )


def test_ffmpeg_refuses_fast_metadata():
    from mtl_engine.ffmpeg import FFmpeg

    if not ip_pools.tx:
        ip_pools.init(1)
    with pytest.raises(ValueError, match="fast_metadata"):
        FFmpeg(app_path="/unused").create_command(
            session_type="st40p",
            fast_metadata=True,
            nic_port_list=["0000:00:01.0", "0000:00:01.1"],
            input_file=PAYLOAD,
            framerate="p59",
        )
