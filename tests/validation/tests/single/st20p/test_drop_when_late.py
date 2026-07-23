# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest

from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422rfc10

pytestmark = pytest.mark.verified

# Start the user-pacing clock one second in the past. With user pacing enabled
# this makes roughly one second's worth of frames (~fps) already late when the
# session starts transmitting; ST20P_TX_FLAG_DROP_WHEN_LATE must drop that
# backlog before the schedule catches up to real time and frames flow normally.
LATE_OFFSET_NS = -1_000_000_000

# fps label -> nominal frames per second, used for the "around fps" bound.
_FPS_VALUE = {
    "p25": 25,
    "p30": 30,
    "p50": 50,
    "p60": 60,
}


@pytest.mark.nightly
@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
    ],
)
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p"]],
    indirect=["media_file"],
    ids=["ParkJoy_1080p"],
)
@pytest.mark.parametrize("fps", ["p25", "p30", "p50", "p60"])
def test_st20p_drop_when_late(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    fps,
    media_file,
):
    """Test that ST20P drop-when-late drops late frames.

    With the user-pacing clock started a second in the past every frame is
    late, so the TX pipeline must drop frames. Assert at least one frame-rate's
    worth (>= fps) was dropped. The drop count is read via the app-agnostic
    ``count_tx_dropped_frames`` helper so the test stays framework-neutral.
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    # Run longer than one MTL stats interval (10s) so at least one TX_st20p
    # stats line reporting the dropped backlog is emitted.
    test_time = 15

    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=fps,
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        input_file=media_file_path,
        test_time=test_time,
        user_pacing=True,
        drop_when_late=True,
        user_time_offset=LATE_OFFSET_NS,
    )

    # Late frames deliberately break RX continuity, so do not let the default
    # RX/TX result gate fail the test; assert on the drop count directly.
    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, fail_on_error=False
    )

    dropped = app.count_tx_dropped_frames()
    assert dropped >= 0, "no TX pipeline stats line found in application output"

    # With the pacing clock a full second behind, at least one frame-rate's
    # worth of late frames must have been dropped.
    expected = _FPS_VALUE[fps]
    assert dropped >= expected, (
        f"dropped only {dropped} frames, expected at least fps={expected} "
        f"(drop-when-late not active?)"
    )
