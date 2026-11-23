# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""IPMX Info Block version on a parameter change (TP-1 13.2 and 13.3).

An IPMX Sender bumps the Info Block version when its media parameters change.
In MTL the application owns the version: RxTxApp never re-creates a session,
so it takes the version from the JSON key ``ipmx_block_version`` and the
caller bumps it. The case sends RGB 8-bit with version 1, then RGB 10-bit with
version 2, and checks that the Sender Reports on the wire and the receiver
carry the new version with the new parameters, and that both streams pass the
VSF scripts. The two runs are two sessions, not one sender updated live.
"""

import re

import pytest
from tests.single.ipmx import ipmx_vsf

_VERSION_RE = re.compile(r", version (\d+),")

# (file format, transport format, depth, bytes per pixel of the file, version)
_RUNS = [
    ("RGB8", "RGB_8bit", 8, 3, 1),
    ("RGBRFC4175PG4BE10", "RGB_10bit", 10, 3.75, 2),
]


@pytest.mark.nightly
def test_ipmx_block_version(
    hosts,
    mtl_path,
    test_time,
    test_config,
    setup_interfaces,
    app_factory,
    ipmx_tools,
    ipmx_capture,
    ipmx_media,
):
    """A new format with a bumped version must reach the receiver as that version."""
    host = list(hosts.values())[0]
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    width, height = 1920, 1080
    for file_fmt, transport_fmt, depth, bpp, version in _RUNS:
        frame_bytes = int(width * height * bpp)
        media = ipmx_media(f"{width}x{height}_{file_fmt}.raw", frame_bytes)
        app = ipmx_vsf.create_st20p(
            app_factory, interfaces, media, width, height, "p60", file_fmt,
            transport_fmt, test_time,
        )  # fmt: skip
        sr_lines, sdp, reports = ipmx_vsf.run_sender(
            app, host, mtl_path, test_time, ipmx_tools, ipmx_capture, "st20p", "raw",
            frame_bytes, width, height, "60", "RGB", depth,
            tag=f"v{version}", block_version=version,
        )  # fmt: skip
        # The first SR on the wire and the RX side carry the version the TX
        # was given, and the published SDP the new depth (TP-1 13.2, 13.3).
        assert [r.block_version for r in reports] == [version]
        versions = {int(m.group(1)) for m in map(_VERSION_RE.search, sr_lines) if m}
        assert versions == {version}, sr_lines
        assert f"depth={depth};" in sdp, sdp
