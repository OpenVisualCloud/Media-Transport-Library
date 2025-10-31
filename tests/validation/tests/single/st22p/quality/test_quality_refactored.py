# SPDX-License-Identifier: BSD-3-Clause

import os
import shutil

import pytest
from mtl_engine.media_files import yuv_files_422p10le
from mtl_engine.rxtxapp import RxTxApp


@pytest.mark.refactored
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    ids=["Penguin_1080p"],
)
@pytest.mark.parametrize("quality", ["quality", "speed"])
@pytest.mark.nightly
def test_quality_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
    quality,
    test_config,
    prepare_ramdisk,
    media_file,
):
    media_file_info = media_file
    host = list(hosts.values())[0]

    app = RxTxApp(app_path="./tests/tools/RxTxApp/build")
    input_path = (
        os.path.join(media, media_file_info["filename"])
        if media
        else media_file_info["filename"]
    )
    # Ensure kahawai.json (plugin configuration) is available in build cwd so st22 encoder can load plugins
    kahawai_src = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "../../../../../..", "kahawai.json")
    )
    kahawai_dst = os.path.join(build, "kahawai.json")
    try:
        if os.path.exists(kahawai_src) and not os.path.exists(kahawai_dst):
            shutil.copy2(kahawai_src, kahawai_dst)
    except Exception as e:
        print(f"Warning: failed to stage kahawai.json into build dir: {e}")
    app.create_command(
        session_type="st22p",
        test_mode="multicast",
        nic_port_list=host.vfs,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        codec="JPEG-XS",
        quality=quality,
        pixel_format=media_file_info["file_format"],
        input_file=input_path,
        codec_threads=2,
        test_time=test_time,
    )
    result = app.execute_test(build=build, test_time=test_time, host=host)
    # Enforce result to avoid silent pass when validation fails
    assert (
        result
    ), "Refactored st22p quality test failed validation (TX/RX outputs or return code)."
