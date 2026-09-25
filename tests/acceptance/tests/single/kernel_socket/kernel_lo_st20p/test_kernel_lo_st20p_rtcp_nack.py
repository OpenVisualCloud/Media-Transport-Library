# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST20P TX with RTCP stays intact under crafted NACKs (kernel loopback).

An st20p session with RTCP streams over ``kernel:lo`` in two cases:

* ``stock``: no NACK sender. The RX loses no packet on loopback, so the TX
  must see no NACK, drop nothing, and retransmit nothing.
* ``crafted``: the test host sends malformed and hostile RTCP NACKs to the TX
  RTCP port (``udp_port + 1``): length underflow, a length past the received
  bytes, runts, wrong flags and name, ``follow`` values past the ring, and a
  256-FCI full-ring repeat. The TX must drop and count the invalid ones and
  parse the others.

In both cases the RX recording must be identical to its source.

The test runs on RxTxApp only. The FFmpeg and GStreamer plugins have no RTCP
option.
"""

import pytest
from mtl_engine.config.universal_params import UNIVERSAL_PARAMS
from mtl_engine.media_files import yuv_files_422rfc10
from mtl_engine.rtcp_nack import start_rtcp_nack_injector
from mtl_engine.rxtxapp import RxTxApp

RTCP_RING_SIZE = 1024  # ST_TX_VIDEO_RTCP_RING_SIZE, RxTxApp sets no buffer_size


@pytest.mark.nightly
@pytest.mark.refactored
@pytest.mark.parametrize("nacks", ["stock", "crafted"])
@pytest.mark.parametrize("test_mode", ["kernel"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p"]],
    indirect=["media_file"],
    ids=["ParkJoy_1080p"],
)
def test_kernello_st20p_rtcp_nack(
    hosts,
    mtl_path,
    test_time,
    test_mode,
    nacks,
    media_file,
    application,
    output_files,
    media_integrity,
):
    """The TX counts no NACK without a sender, and survives crafted NACKs.

    :param hosts: Mapping of host objects from the topology configuration.
    :param mtl_path: Path to the MTL build directory on the remote host.
    :param test_time: Duration to run the streaming pipeline, in seconds.
    :param test_mode: Transport mode parameter (``kernel``).
    :param nacks: ``stock`` sends no NACK, ``crafted`` runs the injector.
    :param media_file: Parametrized media file fixture (info dict, file path).
    :param application: Media application driver fixture (currently ``RxTxApp``).
    :param output_files: Tracker that removes the RX recordings afterwards.
    :param media_integrity: Compares the RX recording with its source.
    """
    if not isinstance(application, RxTxApp):
        pytest.skip("RTCP needs RxTxApp: the FFmpeg and GStreamer plugins have no RTCP")
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    # Kernel-socket loopback init is slower than VF.
    test_time = max(test_time, 90)
    udp_port = UNIVERSAL_PARAMS["port"]

    application.create_command(
        session_type="st20p",
        nic_port_list=["kernel:lo", "kernel:lo"],
        test_mode=test_mode,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        output_file=f"{media_file_path}.out",
        port=udp_port,
        enable_rtcp=True,
        test_time=test_time,
    )
    for path in application.rx_output_files():
        output_files.register(path)

    injector = None
    if nacks == "crafted":
        injector = start_rtcp_nack_injector(
            host, "127.0.0.1", udp_port + 1, duration=test_time
        )
    try:
        application.execute_test(
            build=mtl_path,
            test_time=test_time,
            host=host,
            integrity=media_integrity,
        )
    finally:
        if injector:
            injector.wait(timeout=60)

    stats = application.rtcp_tx_stats()
    assert stats is not None, "no rtcp_tx_stat line: RTCP TX did not start"
    assert stats["rtp_sent"] > 0, f"RTCP TX buffered no RTP packet: {stats}"

    if nacks == "stock":
        # Loopback loses no packet, so the RX sends no NACK.
        assert stats["nack_recv"] == 0, f"NACK without a sender: {stats}"
        assert stats["nack_drop_invalid"] == 0, f"drop without a sender: {stats}"
        assert stats["retransmit_succ"] == 0, f"retransmit without a NACK: {stats}"
        return

    # Each batch adds 7 to nack recv and 6 to nack drop invalid: the three
    # bad-len NACKs count in both, and the kernel socket drops the empty
    # datagram before the parser. Zero in either count means no crafted NACK
    # reached the parser, e.g. the kernel-socket RX path left the IPv4 IHL at 0.
    assert stats["nack_recv"] > 0, f"no crafted NACK reached the TX: {stats}"
    assert stats["nack_drop_invalid"] > 0, f"invalid NACKs not counted: {stats}"
    # One NACK retransmits at most the ring once, however many FCIs repeat it.
    assert (
        stats["retransmit_succ"] <= stats["nack_recv"] * RTCP_RING_SIZE
    ), f"retransmit amplification over one ring per NACK: {stats}"
