# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

from mtl_engine.rxtxapp import RxTxApp

PAYLOAD = "/mnt/ramdisk/media/payload.txt"


def _config(**params) -> dict:
    app = RxTxApp(app_path="/unused")
    app.set_params(
        nic_port_list=["0000:00:01.0", "0000:00:01.1"],
        test_mode="unicast",
        **params,
    )
    return app._create_rxtxapp_config_dict()


def test_fast_metadata_is_an_ancillary_session_opt_in():
    config = _config(
        session_type="ancillary",
        fast_metadata=True,
        fastmetadata_data_item_type=0x3FFFFF,
        fastmetadata_k_bit=1,
        type_mode="rtp",
        ancillary_fps="p50",
        ancillary_url=PAYLOAD,
        payload_type=120,
    )

    tx = config["tx_sessions"][0]["ancillary"][0]
    rx = config["rx_sessions"][0]["ancillary"][0]
    for session in (tx, rx):
        assert session["fast_metadata"] is True
        assert session["fastmetadata_data_item_type"] == 0x3FFFFF
        assert session["fastmetadata_k_bit"] == 1
        assert session["payload_type"] == 120
        # TX sends the file, RX compares every data item against it.
        assert session["ancillary_url"] == PAYLOAD
    assert tx["type"] == "rtp"
    assert tx["ancillary_fps"] == "p50"
    assert all("fastmetadata" not in group for group in config["tx_sessions"])
    assert all("fastmetadata" not in group for group in config["rx_sessions"])


def test_plain_ancillary_session_carries_no_fast_metadata_keys():
    config = _config(session_type="ancillary", ancillary_url=PAYLOAD)

    tx = config["tx_sessions"][0]["ancillary"][0]
    rx = config["rx_sessions"][0]["ancillary"][0]
    for session in (tx, rx):
        assert not any(
            key.startswith(("fast_metadata", "fastmetadata")) for key in session
        )
    assert "ancillary_url" not in rx
