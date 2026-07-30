# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""GStreamer ST20P TX finalize / zero-copy refcount acceptance tests.

Validates the finalize grace period added to ``gst_mtl_st20p_tx``: on shutdown
the element must wait for the in-flight zero-copy GstBuffers (tracked by the
``pending_gst_buffers`` refcount) to drain before tearing down the MTL session,
so the transport never frees a frame slot the NIC still owns.

The ``v210`` / ``I422_10LE`` inputs always select the zero-copy path
(``st_frame_fmt_to_transport()`` maps neither to the hardcoded
``ST20_FMT_YUV_422_10BIT`` transport format, so ``sink->zero_copy`` is true),
and that is the only path that arms the refcount. A clean EOS-driven shutdown
after real streaming must therefore leave NO ``refcnt not zero`` (the MTL
data-plane guard) and NO finalize-timeout warning in the TX log.

Unit-level coverage of the same contract lives in
``tests/unit/gstreamer/gst_mtl_st20p_tx_refcnt_test.cpp`` and
``gst_mtl_st20p_tx_finalize_test.cpp``; these tests exercise it end-to-end on a
real VF pair.
"""

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import GstreamerApp
from mtl_engine.media_files import gstreamer_formats

# The GStreamer ST20P TX element hardcodes transport_fmt = ST20_FMT_YUV_422_10BIT
# and sets zero_copy = (transport_fmt != st_frame_fmt_to_transport(input_fmt)).
# Both catalogued GStreamer inputs (v210 -> ST_FRAME_FMT_V210, I422_10LE ->
# ST_FRAME_FMT_YUV422PLANAR10LE) fall through st_frame_fmt_to_transport()'s
# switch to ST20_FMT_MAX, so zero_copy is always true for them -- and that is the
# only path that arms the pending_gst_buffers refcount. These are exactly the
# proven-good 1920x1080@60 params the video_format suite streams, so we reuse the
# catalog instead of re-deriving (v210 in particular requires width % 6 == 0 and
# is broken at 720p per SDBQ-1971).
ZERO_COPY_FORMATS = gstreamer_formats

WORK_DIR = "/tmp/mtl_gst_st20p_refcnt"


@pytest.mark.nightly
@pytest.mark.parametrize("fmt_key", list(ZERO_COPY_FORMATS.keys()))
def test_st20p_tx_finalize_refcnt_drains(
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    fmt_key,
):
    """A zero-copy ST20P TX pipeline must finalize without leaking refcounts.

    Streams a short synthetic clip TX -> RX on a single host, lets TX reach EOS
    on its own (triggering the finalize grace drain), and asserts the TX log is
    free of the refcount-violation markers.
    """
    spec = ZERO_COPY_FORMATS[fmt_key]
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    host.connection.execute_command(f"mkdir -p {WORK_DIR}", expected_return_codes=None)

    gst_format = GstreamerApp.video_format_change(spec["format"])
    input_file_path = media_create.create_video_file(
        width=spec["width"],
        height=spec["height"],
        framerate=spec["fps"],
        format=gst_format,
        media_path=WORK_DIR,
        output_path=f"refcnt_{fmt_key}.yuv",
        duration=2,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st20p_tx_pipeline(
        build=mtl_path,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        width=spec["width"],
        height=spec["height"],
        framerate=spec["fps"],
        format=gst_format,
        tx_payload_type=112,
        tx_queues=4,
        rx_queues=1,
    )

    # RX only needs to pull frames so TX paces normally; discard the payload.
    rx_config = GstreamerApp.setup_gstreamer_st20p_rx_pipeline(
        build=mtl_path,
        nic_port_list=interfaces_list[1],
        output_path="/dev/null",
        width=spec["width"],
        height=spec["height"],
        framerate=spec["fps"],
        format=gst_format,
        rx_payload_type=112,
        rx_queues=4,
        tx_queues=1,
    )

    try:
        result = GstreamerApp.execute_st20p_tx_finalize_grace(
            build=mtl_path,
            tx_command=tx_config,
            rx_command=rx_config,
            host=host,
        )
    finally:
        media_create.remove_file(input_file_path, host=host)

    assert result["tx_clean_exit"], (
        "TX gst-launch did not exit cleanly on EOS "
        f"(return_code={result['tx_return_code']}); the finalize grace path may "
        "not have run"
    )
    assert not result["violations"], (
        "ST20P TX zero-copy refcount / finalize grace-period was violated on "
        f"shutdown: {result['violations']}"
    )
