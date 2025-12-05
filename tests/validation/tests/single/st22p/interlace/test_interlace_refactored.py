# SPDX-License-Identifier: BSD-3-Clause

import os

import pytest
from mtl_engine.media_files import yuv_files_interlace
from mtl_engine.rxtxapp import RxTxApp


@pytest.mark.refactored
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_interlace.values()),
    ids=list(yuv_files_interlace.keys()),
)
def test_interlace_refactored(
    hosts,
    build,
    media,
    nic_port_list,
    test_time,
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
    app.create_command(
        session_type="st22p",
        test_mode="multicast",
        nic_port_list=host.vfs,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        codec="JPEG-XS",
        quality="speed",
        pixel_format=media_file_info["file_format"],
        input_file=input_path,
        codec_threads=2,
        interlaced=True,
        test_time=test_time,
    )
    app.execute_test(build=build, test_time=test_time, host=host)
