import pytest

pytestmark = pytest.mark.verified


# Map each test case to its media file and properties
MEDIA_FILES = [
    {
        "filename": "1920x1080p10le_1.yuv",
        "width": 1920,
        "height": 1080,
        "fps": 25,
        "file_format": "yuv422p10le",
    },
    {
        "filename": "1920x1080p10le_1.yuv",
        "width": 1920,
        "height": 1080,
        "fps": 30,
        "file_format": "yuv422p10le",
    },
    {
        "filename": "1920x1080p10le_1.yuv",
        "width": 1920,
        "height": 1080,
        "fps": 60,
        "file_format": "yuv422p10le",
    },
    {
        "filename": "3840x2160p10le_1.yuv",
        "width": 3840,
        "height": 2160,
        "fps": 30,
        "file_format": "yuv422p10le",
    },
    {
        "filename": "3840x2160p10le_1.yuv",
        "width": 3840,
        "height": 2160,
        "fps": 60,
        "file_format": "yuv422p10le",
    },
]

FORMAT_MEDIA = [
    ("i1080p25", "YUV_422_10bit"),
    ("i1080p30", "YUV_422_10bit"),
    ("i1080p60", "YUV_422_10bit"),
    ("i2160p30", "YUV_422_10bit"),
    ("i2160p60", "YUV_422_10bit"),
]

@pytest.mark.parametrize("application", ["ffmpeg", "rxtxapp"])
@pytest.mark.parametrize("output_format", ["yuv", "h264"])
@pytest.mark.parametrize(
    "video_format, transport_format, media_file",
    [
        (fmt[0], fmt[1], MEDIA_FILES[i])
        for i, fmt in enumerate(FORMAT_MEDIA)
    ],
    ids=[f"{fmt[0]}_{fmt[1]}" for fmt in FORMAT_MEDIA],
    indirect=["media_file"],
)
def test_format(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces,
    test_time,
    test_config,
    media_file,
    video_format,
    transport_format,
    output_format,
):
    if output_format == "h264" and application == "rxtxapp":
        pytest.skip("RxTxApp does not support h264 output format")
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=interfaces_list,
        test_mode="multicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        pixel_format=media_file_info["file_format"],
        transport_format=transport_format,
        input_file=media_file_path,
        output_format=output_format,
        test_time=test_time,
    )
    result = app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
    )
    assert result, f"Format test failed for {video_format} → {transport_format} ({output_format})"
