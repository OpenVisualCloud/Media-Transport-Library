# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST20 RTP-level TX user pacing, end to end.

The application builds the RTP packets itself and gives each frame's first
packet a TAI with st20_tx_put_mbuf_tai(). The library must put the frame on
that time:

- user pacing: the epoch nearest the TAI. RxTxApp asks for the epoch itself,
  so the frame lands where the default pacing would put it (TR_offset after
  the epoch);
- exact user pacing: the first packet at the TAI itself. RxTxApp asks for half
  a frame period after the epoch, far from the default start, so the FPT
  plainly shows the TAI was honoured.

The RX timing parser of the receiving MTL session measures where each frame's
first packet lands after its epoch (FPT), which tells the two modes apart on
the wire. A refused or missing TAI would count stat_error_user_timestamp and
fall back to the default pacing, so every case also asserts that count is 0.

Only RxTxApp exposes RTP-level TX.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422rfc10

pytestmark = [pytest.mark.nightly, pytest.mark.tx_side]

# RxTxApp's name for progressive 1080p59.94; the "i" does not mean interlaced
VIDEO_FORMAT = "i1080p59"
# 1080p59.94: the frame period, and the exact TAI RxTxApp asks for.
FRAME_NS = 1_000_000_000 * 1001 / 60000
EXACT_FPT_NS = FRAME_NS / 2
# The exact start is software-timed (TSC wait, then the first packet); a few
# us late is normal, 100 us is far outside it.
EXACT_FPT_TOLERANCE_NS = 100_000
# The default start is TR_offset - VRX x TRS, well under a quarter frame.
DEFAULT_FPT_MAX_NS = FRAME_NS / 4

# The three ways to pace the frames, run through RxTxApp. "default" is the
# baseline: no flag, no TAI. "user_pacing" and "exact_user_pacing" set the TX
# session's flags. FFmpeg and GStreamer drive the pipeline API, which has no
# per-packet TAI: one skip each records that gap.
CASES = [
    pytest.param("rxtxapp", {}, "default", id="rxtxapp-default"),
    pytest.param("rxtxapp", {"user_pacing": True}, "default", id="rxtxapp-user_pacing"),
    pytest.param(
        "rxtxapp", {"exact_user_pacing": True}, "exact", id="rxtxapp-exact_user_pacing"
    ),
    pytest.param(
        "ffmpeg",
        {"exact_user_pacing": True},
        "exact",
        marks=pytest.mark.skip(reason="FFmpeg has no RTP-level ST20 TX"),
        id="ffmpeg-exact_user_pacing",
    ),
    pytest.param(
        "gstreamer",
        {"exact_user_pacing": True},
        "exact",
        marks=pytest.mark.skip(reason="GStreamer has no RTP-level ST20 TX"),
        id="gstreamer-exact_user_pacing",
    ),
]


@pytest.mark.parametrize("application, pacing_params, expected_start", CASES)
# Any 1080p 10-bit 4:2:2 source will do: pacing does not depend on the picture,
# and the TX loops the file. The 24-frame clip is the smallest one.
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p_24frames"]],
    indirect=True,
    ids=["ParkJoy_1080p_24frames"],
)
def test_st20_rtp_user_pacing(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    pacing_params,
    expected_start,
    media_file,
):
    """An RTP-level frame starts where its TAI asks, and no TAI is refused.

    One RTP-level 1080p59.94 TX session and one frame-level RX session on the
    same host. Beyond execute_test()'s own pass/fail check, the case passes
    when:

    - the user_pacing and exact_user_pacing cases really set the flags on the
      TX session;
    - every steady-state stat period received frames, with FPT at the
      epoch + TR_offset for default and user pacing, or at the exact TAI for
      exact user pacing;
    - stat_error_user_timestamp stayed 0.

    The RX timing parser's narrow verdict is not required. RxTxApp's
    RTP-level TX is rated WIDE (VRX about 10, max 19) even with the default
    pacing, on main as well, where its frame-level TX on the same ports is
    NARROW (VRX about 3, max 5): a property of that path, not of user pacing.
    Exact user pacing is rated FAILED, as it starts half a frame late on
    purpose.

    Limitation: RxTxApp's user_pacing leg asks for the very epoch the default
    pacing picks, so on the wire it matches the default leg. It proves the
    flags were set and every TAI was accepted, not that the epoch came from
    the TAI; the exact leg is the one that proves the TAI is honored.
    """
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    app = app_factory(application)
    app.create_command(
        session_type="video",
        type_mode="rtp",
        # the RX timing parser only runs on a frame-level RX session
        rx_type_mode="frame",
        pacing="narrow",
        # Both VFs on one PF: FPT is read against the RX port's clock, and a
        # VF on the other PF reads a clock several ms off the TX one.
        nic_port_list=setup_interfaces.get_interfaces_list_single("2VFxPF", count=2),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        test_mode="multicast",
        video_format=VIDEO_FORMAT,
        pg_format=media_info["format"],
        video_url=media_path,
        rx_timing_parser=True,
        rx_timing_narrow=False,
        test_time=test_time,
        **pacing_params,
    )
    app.execute_test(build=mtl_path, test_time=test_time, host=host)

    if pacing_params:
        assert app.tx_user_pacing_applied(), "the TX session did not take user pacing"

    fpts = app.rx_first_packet_times_ns()
    # The first stat period holds the session start-up and the last one the TX
    # teardown, which can leave it with no frame: judge the periods in between.
    assert len(fpts) >= 3, f"too few RX timing stat periods to judge: FPT {fpts}"
    steady = fpts[1:-1]
    # the parser reports -1 for a stat period with no frame
    assert min(steady) >= 0, f"a stat period received no frame: FPT {steady} ns"
    if expected_start == "exact":
        off = [f for f in steady if abs(f - EXACT_FPT_NS) > EXACT_FPT_TOLERANCE_NS]
        assert not off, (
            f"exact user pacing: FPT {off} ns is not within "
            f"{EXACT_FPT_TOLERANCE_NS} ns of the TAI at {EXACT_FPT_NS:.0f} ns"
        )
    else:
        late = [f for f in steady if f > DEFAULT_FPT_MAX_NS]
        assert not late, (
            f"FPT {late} ns is past {DEFAULT_FPT_MAX_NS:.0f} ns: the frame did "
            "not start on its epoch"
        )

    errors = app.count_tx_user_timestamp_errors()
    assert errors >= 0, "no TX stat dump in the output: run at log_level info"
    assert errors == 0, f"stat_error_user_timestamp counted {errors} frame(s)"
