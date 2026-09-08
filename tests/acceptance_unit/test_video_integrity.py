# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Frame-size table shared by the RX byte oracles and the MD5 integrity check.

A wrong frame size does not surface as a size error -- it shifts every chunk
boundary, so ``check_integrity_file`` reports the whole capture as bad frames.
"""

import pytest
from common.integrity.video_integrity import calculate_yuv_frame_size
from mtl_engine.gstreamer import _st20p_frame_size
from mtl_engine.media_files import yuv_files_input_formats


@pytest.mark.parametrize(
    "file_format, expected",
    [
        ("YUV422PLANAR10LE", 1920 * 1080 * 4),
        ("yuv422p10le", 1920 * 1080 * 4),
        ("YUV422RFC4175PG2BE10", int(1920 * 1080 * 2.5)),
        ("UYVY", 1920 * 1080 * 2),
    ],
)
def test_flat_formats_are_bytes_per_pixel(file_format, expected):
    assert calculate_yuv_frame_size(1920, 1080, file_format) == expected


@pytest.mark.parametrize(
    "width, height",
    [(1920, 1080), (1280, 720), (3840, 2160), (7680, 4320)],
)
def test_v210_is_mtls_compact_packing(width, height):
    """v210 packs 3 pixels per 8 bytes with no row padding.

    These bytes are an MTL RX dump, so the table has to match ``st_frame_size()``
    in lib/src/st2110/st_fmt.c, which computes ``pixels * 8 / 3`` for
    ST_FRAME_FMT_V210, and not GStreamer's 48-pixel-padded row stride.
    """
    assert calculate_yuv_frame_size(width, height, "v210") == width * height * 8 // 3


@pytest.mark.parametrize("width, height", [(1920, 1080), (3840, 2160), (7680, 4320)])
def test_v210_matches_the_padded_stride_at_multiple_of_48_widths(width, height):
    """Cross-check: at a multiple of 48 the two rules must agree exactly.

    Where they do, TX reading the input asset with GStreamer's stride and RX
    dumping MTL's frame size produce the same byte count, so the MD5 comparison
    is meaningful.
    """
    assert width % 48 == 0
    assert calculate_yuv_frame_size(width, height, "v210") == (
        ((width + 47) // 48) * 128 * height
    )


def test_v210_diverges_from_the_padded_stride_at_1280():
    """1280 is the one suite width where the two rules disagree.

    ceil(1280/48) = 27 groups = 3456 B per row against MTL's 3413 1/3, so a
    padded table would over-state a 720p frame by 30720 B. The only v210 path in
    the suite gates on ``width % 6`` (tests/single/gstreamer/video_resolution),
    which excludes 1280 -- but the table must be right regardless of which
    geometries happen to be parametrized today.
    """
    assert 1280 % 48 != 0
    assert calculate_yuv_frame_size(1280, 720, "v210") == 2457600
    assert ((1280 + 47) // 48) * 128 * 720 == 2488320


def test_v210_rejects_a_geometry_mtl_cannot_encode():
    """``st_frame_size()`` refuses a pixel count that is not a multiple of 3.

    Silently returning a truncated size would shift every chunk boundary in
    ``check_integrity_file`` and report a healthy capture as all-bad frames.
    """
    assert (722 * 4) % 3 != 0
    with pytest.raises(ValueError, match="multiple of 3"):
        calculate_yuv_frame_size(722, 4, "v210")


def test_every_selected_input_format_can_be_sized():
    """A registry ``file_format`` the table cannot size is an ungradeable case.

    ``test_input_formats`` passes ``media_file_info["file_format"]`` straight
    through to the integrity runner, so a missing arm surfaces as
    ``Size of <format> pixel is not known`` -- a failure about the table, on a
    run whose data path may have been perfectly healthy.
    """
    for name, entry in yuv_files_input_formats.items():
        size = calculate_yuv_frame_size(
            entry["width"], entry["height"], entry["file_format"]
        )
        assert size > 0, name


@pytest.mark.parametrize(
    "file_format, expected",
    [
        # MTL's own names for bytes the table already knew under a GStreamer or
        # FFmpeg spelling: st_frame_size() routes YUV420CUSTOM8 through
        # st20_frame_size(ST20_FMT_YUV_420_8BIT) (a 6-byte group covering 4
        # pixels, i.e. 1.5 B/px) and sizes YUV422CUSTOM8 as pixels * 2, the same
        # as UYVY.
        ("YUV420CUSTOM8", 1920 * 1080 * 3 // 2),
        ("yuv420p", 1920 * 1080 * 3 // 2),
        ("YUV422CUSTOM8", 1920 * 1080 * 2),
    ],
)
def test_mtl_custom8_names_size_like_their_aliases(file_format, expected):
    assert calculate_yuv_frame_size(1920, 1080, file_format) == expected


def test_unknown_format_raises_rather_than_guessing():
    with pytest.raises(ValueError, match="not known"):
        calculate_yuv_frame_size(1920, 1080, "NoSuchFormat")


@pytest.mark.parametrize("pixel_format", ["YUV422PLANAR10LE", "v210"])
def test_gstreamer_byte_oracle_uses_the_integrity_table(pixel_format):
    """The two must agree or the byte floor and the MD5 chunking disagree."""
    assert _st20p_frame_size(pixel_format, 1920, 1080) == calculate_yuv_frame_size(
        1920, 1080, pixel_format
    )
