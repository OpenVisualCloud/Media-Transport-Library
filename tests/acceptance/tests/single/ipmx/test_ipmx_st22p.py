# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""IPMX ST 2110-22 JPEG XS sender checks of VSF TR-10 TP-1 section 13 and 19.3.

RxTxApp encodes RGB 4:4:4 8-bit or 10-bit frames with the SVT-JPEG-XS MTL
plugin in its IPMX mode (``ipmx_1080p.json`` / ``ipmx_2160p.json`` of the plugin) and
sends one st22p IPMX stream to an IPMX RX session. The wire is captured from
before the start, then the VSF scripts check it:
``ipmx_jxsv_validate_pcap.py --cmax --cfg --sdp`` (13.1, 13.3, 13.6.1, 19.3,
and the CMAX and TR-10-9 11.2 parts of 13.5) and ``sdp_validation_script.py``
(13.2, 13.6.2). The RX side must report its Sender Report line OK. The matrix
is the IPMX JPEG XS MVP scope: 1080p50/59.94/60 and 2160p59.94/60 at 3 and 4
bits per pixel, in RGB 4:4:4 8-bit (``GBRPLANAR8``, the TR-10-15 Part 1
section 8 minimum) and 10-bit (``GBRPLANAR10LE``, an additional bit depth it
allows): the validator then checks bit depth 10 against the Media Info Block.

The SVT encoder threads must not run on the MTL scheduler CPUs, or the TX
frame start drifts (TR-10-9 11.2): isolate the CPUs given in
``test_config.ipmx.lcores`` before the run (see the README, "IPMX"). Without
``lcores`` a TR-10-9 11.2 failure is a pytest warning (known host limitation).
"""

import pytest
from tests.single.ipmx import ipmx_vsf

# One point per case: (width, height, RxTxApp fps, exactframerate,
# plugin config, bits per pixel, pixel format, bit depth). TR-08 levels:
# Sublev3bpp up to 3 bpp, so the 4 bpp points switch the plugin config to
# Sublev4bpp and 4 bpp. The 10-bit points use the same configs and have ids
# prefixed rgb10_; the 8-bit ids are unchanged.
# The TR-10-9 11.2 frame and Sender Report cadence checks need the isolation.
CADENCE_CHECKS = ("TR-10-9-11.2a", "TR-10-9-11.2b")
_ST22P_RATES = [
    (1920, 1080, "p50", "50", "1080p", "1080p50"),
    (1920, 1080, "p59", "60000/1001", "1080p", "1080p59"),
    (1920, 1080, "p60", "60", "1080p", "1080p60"),
    (3840, 2160, "p59", "60000/1001", "2160p", "2160p59"),
    (3840, 2160, "p60", "60", "2160p", "2160p60"),
]
_ST22P_FORMATS = [("GBRPLANAR8", 8, ""), ("GBRPLANAR10LE", 10, "rgb10_")]
ST22P_POINTS = [
    pytest.param(
        w, h, fps, rate, plugin_cfg, bpp, pixel_format, depth,
        id=f"{tag}{rid}_{bpp}bpp",
        marks=[pytest.mark.smoke] if f"{tag}{rid}_{bpp}bpp" == "1080p59_3bpp" else [],
    )
    for (pixel_format, depth, tag) in _ST22P_FORMATS
    for (w, h, fps, rate, plugin_cfg, rid) in _ST22P_RATES
    for bpp in (3, 4)
]  # fmt: skip


@pytest.mark.nightly
@pytest.mark.parametrize(
    "width, height, fps, rate, plugin_cfg, bpp, pixel_format, depth", ST22P_POINTS
)
def test_ipmx_st22p_sender(
    hosts,
    mtl_path,
    test_time,
    test_config,
    setup_interfaces,
    app_factory,
    ipmx_tools,
    ipmx_capture,
    ipmx_media,
    width,
    height,
    fps,
    rate,
    plugin_cfg,
    bpp,
    pixel_format,
    depth,
):
    """An IPMX JPEG XS stream must pass the VSF jxsv and SDP scripts."""
    if not ipmx_tools["svt_plugin_dir"]:
        pytest.skip("test_config.ipmx.svt_plugin_dir not set (SVT-JPEG-XS mtl-plugin)")
    host = list(hosts.values())[0]
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    # Three planes of one byte (8-bit) or two (10-bit) a sample.
    sample = 1 if depth == 8 else 2
    media = ipmx_media(
        f"{width}x{height}_{pixel_format}.raw", width * height * 3 * sample, bits=depth
    )

    # The plugin's shipped IPMX config, with Sublev4bpp and 4 bpp for 4 bpp.
    svt_cfg = f"{ipmx_capture.workdir}/svt_{plugin_cfg}_{bpp}bpp.json"
    sed = (
        """sed 's/"stream_level": "0x\\(..\\)04",/"stream_level": "0x\\106", "bpp": 4,/'"""
        if bpp == 4
        else "cat"
    )
    host.connection.execute_command(
        f"{sed} {ipmx_tools['svt_plugin_dir']}/ipmx_{plugin_cfg}.json > {svt_cfg}",
        shell=True,
    )

    app = app_factory("rxtxapp")
    app.create_command(
        session_type="st22p",
        nic_port_list=interfaces,
        test_mode="multicast",
        width=width,
        height=height,
        framerate=fps,
        codec="JPEG-XS",
        pixel_format=pixel_format,
        input_file=media,
        test_time=test_time,
        lcores=ipmx_tools["lcores"],
    )
    # libmtl loads the plugin from the .local_install registry; the plugin
    # reads its config from the environment.
    # The command keeps the env prefix first, so execute_test adds no second one.
    app.command = (
        app.with_local_kahawai("", mtl_path)
        + f"SVT_JXS_MTL_PLUGIN_CONFIG={svt_cfg} {app.command}"
    )
    _, sdp, _ = ipmx_vsf.run_sender(
        app, host, mtl_path, test_time, ipmx_tools, ipmx_capture,
        "st22p", "jxsv", width * height * bpp // 8, width, height, rate, "RGB", depth,
        host_limited=() if ipmx_tools["lcores"] else CADENCE_CHECKS,
    )  # fmt: skip
    # The codestream sublevel the plugin config sets reaches the SDP (TR-08).
    assert f"sublevel=Sublev{bpp}bpp" in sdp, sdp
    # The session bit depth reaches the SDP (TR-10-11 section 10).
    assert f"depth={depth};" in sdp, sdp
