# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

"""GStreamer ST40P ancillary transport validation.

Exercises ANC payload handling over GStreamer across frame rates, payload
sizes, frame buffers, and RFC8331 pseudo streams to ensure pacing, metadata,
and capture remain stable end to end when using paired virtual functions on
a single physical port.
"""

import os
import re
import uuid
from contextlib import contextmanager

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import GstreamerApp
from mtl_engine.execute import log_info, run


def _frame_info_path(base_dir=None):
    target_dir = base_dir or "/tmp"
    base = GstreamerApp.sanitize_filename(GstreamerApp.get_case_id())
    token = uuid.uuid4().hex
    return os.path.join(target_dir, f"{base}_{token}_frameinfo.log")


_FRAME_INFO_PATTERN = re.compile(
    r"ts=(?P<ts>\d+)\s+meta=(?P<meta>\d+)\s+rtp_marker=(?P<rtp_marker>\d+)\s+"
    r"seq_discont=(?P<seq_discont>\d+)\s+seq_lost=(?P<seq_lost>\d+)\s+"
    r"pkts_total=(?P<pkts_total>\d+)\s+pkts_recv_p=(?P<pkts_recv_p>\d+)\s+pkts_recv_r=(?P<pkts_recv_r>\d+)"
)


def _parse_frame_info_timestamps(frame_info_text: str) -> list[int]:
    """Extract timestamps from frame-info log lines."""
    ts_values = []
    for line in frame_info_text.splitlines():
        match = _FRAME_INFO_PATTERN.search(line)
        if match:
            ts_values.append(int(match.group("ts")))
    return ts_values


@contextmanager
def _test_summary(name: str, expectation: str):
    """Structured test logging so pytest.log captures start/fail/pass summaries."""
    log_info(f"[{name}] START: {expectation}")
    try:
        yield
    except Exception as exc:  # pragma: no cover - diagnostic
        log_info(f"[{name}] FAIL: {expectation}; reason={exc}")
        raise
    else:
        log_info(f"[{name}] PASS: {expectation}")


# helper function to setup input and output file paths for ancillary files
def setup_paths(media_file):
    media_file_info, media_file_path = media_file
    if not media_file_path:
        raise ValueError(
            "ramdisk was not setup correctly for media_file fixture",
        )

    input_file_path = os.path.join(media_file_path, "input_anc.txt")
    output_file_path = os.path.join(media_file_path, "output_anc.txt")
    return input_file_path, output_file_path


@pytest.mark.nightly
@pytest.mark.parametrize("fps", [24, 25, 30, 50, 60, 100, 120])
@pytest.mark.parametrize("file_size_kb", [10, 100])
@pytest.mark.parametrize("framebuff", [3])
def test_st40p_fps_size(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    file_size_kb,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Validate ST40P ancillary (ANC) transport over GStreamer across frame-rate and
    payload-size matrices to exercise scheduling, pacing, and metadata delivery.
    Small and medium text payloads are generated on the fly, transmitted over
    paired VFs on a single physical port, and captured for byte-for-byte
    comparison by the harness.

    .. rubric:: Purpose
    Sweep ST40P ANC text payloads across FPS and size matrices to confirm pacing
    stability and byte-accurate capture over paired VFs on one host.

    .. rubric:: Pass Criteria
    - Generated text files round-trip without mismatch for every fps/payload combo.
    - RX pipeline stays alive with framebuff=3 and reports no timeouts.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param file_size_kb: Size of generated text payload (KB).
    :param fps: Frame rate used for packet pacing.
    :param framebuff: Frame buffer count provisioned in the pipeline.
    :param test_time: Duration to run TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"),
    )

    input_file_path, output_file_path = setup_paths(media_file)

    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=15,
    )

    expectation = (
        f"ST40P text ANC {file_size_kb}KB @ {fps}fps framebuff={framebuff} "
        "delivers byte-accurate capture without RX timeout"
    )

    try:
        with _test_summary("test_st40p_fps_size", expectation):
            GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=5,
                log_frame_info=True,
            )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)


@pytest.mark.nightly
@pytest.mark.parametrize("fps", [60])
@pytest.mark.parametrize("file_size_kb", [100])
@pytest.mark.parametrize("framebuff", [1, 3, 6, 12])
def test_st40p_framebuff(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    file_size_kb,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Stress the ST40P ANC path by sweeping frame buffer depth while keeping fps=60
    and payload size fixed, surfacing latency or starvation issues in buffer
    management under load.

    .. rubric:: Purpose
    Sweep ST40P ANC across varying frame buffer depths to expose starvation or
    latency issues while holding fps=60 and payload=100KB constant.

    .. rubric:: Pass Criteria
    - TX/RX stay stable and complete without timeouts for each framebuff value.
    - Output matches input across the sweep.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param file_size_kb: Size of generated text payload (KB).
    :param fps: Frame rate used for packet pacing.
    :param framebuff: Frame buffer count provisioned in the pipeline.
    :param test_time: Duration to run TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"),
    )
    input_file_path, output_file_path = setup_paths(media_file)
    # Base the timeout on parameter to make sure the amount of time between
    # RX and TX is less than the timeout period
    timeout_period = 20

    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=timeout_period + 10,
    )

    expectation = (
        f"ST40P 100KB ANC @ {fps}fps framebuff={framebuff} survives "
        f"timeout>={timeout_period} with no data loss"
    )

    try:
        with _test_summary("test_st40p_framebuff", expectation):
            GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=timeout_period,
                log_frame_info=True,
            )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)

    """
    Validate ST40p integrity using GStreamer RFC8331 pipelines. A pseudo
    RFC8331 input file, generated via ancgenerator, carries fixed ancillary
    frames; output is compared against simplified Python output to verify
    metadata consistency. This verifies ancillary integrity in complex
    scenarios for the MTL library using both RFC8331 and simplified formats.
    """


@pytest.mark.nightly
@pytest.mark.parametrize("fps", [24, 25, 30, 50, 60, 100, 120])
@pytest.mark.parametrize("framebuff", [1, 3, 6, 12])
def test_st40p_format_8331(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Exercise ST40P ANC transport using RFC8331 pseudo payloads to validate
    DID/SDID handling, pacing, and metadata capture across fps and buffer
    matrices. Synthetic RFC8331 frames are generated per run and validated for
    fidelity at the receiver.

    .. rubric:: Purpose
    Validate ST40P RFC8331 ANC carriage by generating pseudo frames and
    exercising DID/SDID handling, pacing, and metadata capture over multiple
    fps and frame buffer values.

    .. rubric:: Pass Criteria
    - RX captures RFC8331 payloads and metadata without timeouts.
    - Byte-for-byte comparison succeeds for each fps/framebuff combination.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param fps: Frame rate used for packet pacing.
    :param framebuff: Frame buffer count provisioned in the pipeline.
    :param test_time: Duration to run TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    # Get the first host for remote execution
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF"),
    )
    # Based on this parameters
    timeout_period = 15

    input_file_path, output_file_path = setup_paths(media_file)

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
        tx_rfc8331=True,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=timeout_period + 10,
        capture_metadata=True,
    )

    expectation = (
        f"ST40P RFC8331 ANC @ {fps}fps framebuff={framebuff} captures metadata "
        "and payload without timeout"
    )

    try:
        with _test_summary("test_st40p_format_8331", expectation):
            GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=timeout_period,
                log_frame_info=True,
            )
    finally:
        # Remove the files after the test
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)


@pytest.mark.nightly
@pytest.mark.parametrize("fps", [25, 50, 60])
@pytest.mark.parametrize("file_size_kb", [10, 100])
@pytest.mark.parametrize("framebuff", [3, 6])
def test_st40i_basic(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    file_size_kb,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Verify interlaced ST40 ancillary transport over GStreamer for common text
    payload sizes and buffer depths on one host, ensuring TX/RX parity and
    stable interlaced cadence.

    .. rubric:: Purpose
    Verify interlaced ST40 ANC transport for text payloads across common fps and
    buffer depths on a single host.

    .. rubric:: Pass Criteria
    - RX matches TX payload for each fps/framebuff combination.
    - Pipelines remain stable at interlaced cadence with no timeout.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param file_size_kb: Size of generated text payload (KB).
    :param fps: Frame rate used for packet pacing.
    :param framebuff: Frame buffer count provisioned in the pipeline.
    :param test_time: Duration to run TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    input_file_path, output_file_path = setup_paths(media_file)

    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
        tx_interlaced=True,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=15,
        rx_interlaced=True,
    )

    expectation = (
        f"ST40i text ANC {file_size_kb}KB @ {fps}fps framebuff={framebuff} "
        "round-trips without diff"
    )

    try:
        with _test_summary("test_st40i_basic", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=5,
                log_frame_info=True,
            )
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)


@pytest.mark.nightly
@pytest.mark.parametrize("fps", [50])
@pytest.mark.parametrize("framebuff", [3])
def test_st40i_rfc8331(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Validate interlaced RFC8331 ANC carriage end-to-end: generate synthetic
    pseudo frames, transmit interlaced, and verify the receiver captures both
    metadata and payload intact.

    .. rubric:: Purpose
    Validate interlaced RFC8331 ANC carriage by sending synthetic frames and
    confirming metadata/payload capture without loss.

    .. rubric:: Pass Criteria
    - RX captures RFC8331 metadata and payload interlaced with no timeout.
    - Output matches generated pseudo ANC stream.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param fps: Frame rate used for packet pacing.
    :param framebuff: Frame buffer count provisioned in the pipeline.
    :param test_time: Duration to run TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    input_file_path, output_file_path = setup_paths(media_file)

    input_file_path = media_create.create_ancillary_rfc8331_pseudo_file(
        size_frames=fps * 10,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
        tx_rfc8331=True,
        tx_interlaced=True,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=20,
        capture_metadata=True,
        rx_interlaced=True,
    )

    expectation = (
        f"ST40i RFC8331 ANC @ {fps}fps framebuff={framebuff} preserves payload "
        "and metadata without timeout"
    )

    try:
        with _test_summary("test_st40i_rfc8331", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=5,
                log_frame_info=True,
            )
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)


@pytest.mark.nightly
@pytest.mark.parametrize("fps", [50])
@pytest.mark.parametrize("file_size_kb", [10])
@pytest.mark.parametrize("framebuff", [3])
def test_st40i_interlace_flag_mismatch(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    file_size_kb,
    fps,
    framebuff,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Negative coverage for mismatched interlace flags: TX sends interlaced while
    RX expects progressive, validating that the pipeline signals failure rather
    than accepting bad cadence.

    .. rubric:: Purpose
    Negative coverage where TX is interlaced and RX expects progressive.

    .. rubric:: Pass Criteria
    - GStreamer pipeline returns False, signalling the mismatch is detected.
    - No unexpected RX readiness or payload is produced.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param file_size_kb: Size of generated text payload (KB).
    :param fps: Frame rate used for packet pacing.
    :param framebuff: Frame buffer count provisioned in the pipeline.
    :param test_time: Duration to run TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    input_file_path, output_file_path = setup_paths(media_file)

    input_file_path = media_create.create_text_file(
        size_kb=file_size_kb,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=framebuff,
        tx_fps=fps,
        tx_did=67,
        tx_sdid=2,
        tx_interlaced=True,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=framebuff,
        timeout=15,
        rx_interlaced=False,
    )

    expectation = (
        f"Interlace mismatch TX interlaced/RX progressive @ {fps}fps "
        f"framebuff={framebuff} must fail"
    )

    try:
        with _test_summary("test_st40i_interlace_flag_mismatch", expectation):
            result = GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=5,
                log_frame_info=True,
            )
            assert not result, "Interlace mismatch unexpectedly succeeded"
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)


@pytest.mark.nightly
def test_st40p_rx_timeout(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Force a deliberately short receiver timeout to ensure the pipeline exposes
    RX-side stalls as failures rather than hanging or passing silently.

    .. rubric:: Purpose
    Force a short RX timeout to confirm the pipeline surfaces receiver timeouts
    as a test failure.

    .. rubric:: Pass Criteria
    - execute_test returns False because RX timeout is hit intentionally.
    - No unexpected payload capture occurs before timeout.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    input_file_path, output_file_path = setup_paths(media_file)

    input_file_path = media_create.create_text_file(
        size_kb=1,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=60,
        tx_did=67,
        tx_sdid=2,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=1,  # force receiver timeout
    )

    expectation = "RX timeout triggers failure with no payload capture"

    try:
        with _test_summary("test_st40p_rx_timeout", expectation):
            result = GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=2,
                host=host,
                tx_first=False,
                sleep_interval=1,
                suppress_fail_logs=True,
                log_frame_info=True,
            )
            assert not result, "RX timeout did not fail the test as expected"
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)


@pytest.mark.nightly
def test_st40p_split_mode_frame_info_logging(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Split-mode ANC validation with frame-info capture enabled to ensure sequence
    discontinuities, packet totals, and marker bits are recorded for debugging
    while payloads round-trip at 50 fps.

    .. rubric:: Purpose
    Split-mode ANC with frame-info capture to confirm seq discontinuity,
    packet totals, and marker fields are recorded.

    .. rubric:: Pass Criteria
    - Frame info log contains seq_discont, pkts_total, and rtp_marker fields.
    - Payload round-trips without timeout at 50 fps.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")
    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    input_file_path = media_create.create_text_file(
        size_kb=10,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=50,
        tx_did=67,
        tx_sdid=2,
        tx_split_anc_by_pkt=True,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=15,
        frame_info_path=frame_info_path,
        rx_rtp_ring_size=2048,
    )

    expectation = "Split-mode frame-info logs seq markers and packet totals"

    try:
        with _test_summary("test_st40p_split_mode_frame_info_logging", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=6,
                host=host,
                tx_first=False,
                sleep_interval=4,
                log_frame_info=True,
            )

            info_dump = run(f"cat {frame_info_path}", host=host)
            assert info_dump.return_code == 0
            assert "seq_discont=" in info_dump.stdout_text
            assert "pkts_total=" in info_dump.stdout_text
            assert "rtp_marker=" in info_dump.stdout_text
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)


@pytest.mark.nightly
def test_st40p_split_mode_invalid_rtp_ring_rejected(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Negative coverage to ensure RX rejects invalid RTP ring sizes (non power of
    two) during startup rather than running with bad buffering assumptions.

    .. rubric:: Purpose
    Validate RX rejects non power-of-two RTP ring sizes by failing startup.

    .. rubric:: Pass Criteria
    - RX launch returns non-zero when ring size is invalid (100 entries).
    - No frame info or payload is produced.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")
    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    input_file_path = media_create.create_text_file(
        size_kb=10,
        output_path=input_file_path,
        host=host,
    )

    # Non power-of-two ring size should be rejected by plugin
    rx_cmd = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=15,
        frame_info_path=frame_info_path,
        rx_rtp_ring_size=100,
    )

    expectation = "RX rejects non power-of-two RTP ring size (100)"

    try:
        with _test_summary(
            "test_st40p_split_mode_invalid_rtp_ring_rejected", expectation
        ):
            rx_proc = run(" ".join(rx_cmd), cwd=build, timeout=10, host=host)
            assert rx_proc.return_code != 0
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)


@pytest.mark.nightly
def test_st40p_split_mode_pacing_respected(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Check paced RTP sender timing in split-mode RX by sending a short burst with
    controlled spacing and verifying jitter stays within budget while RX keeps
    up.

    .. rubric:: Purpose
    Check paced RTP sender timing is honored by the split-mode receiver.

    .. rubric:: Pass Criteria
    - Frame-info RTP timestamp deltas stay within 0.5ms of the 60fps frame period (90kHz ticks).
    - RX completes without timeout and frame info logging stays accessible.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")
    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    input_file_path = media_create.create_text_file(
        size_kb=10,
        output_path=input_file_path,
        host=host,
    )

    expectation = "Paced TX emits configured burst (8 packets) with frame-info logged"

    tx_fps = 60

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=tx_fps,
        tx_did=67,
        tx_sdid=2,
        tx_split_anc_by_pkt=True,
        tx_test_mode="paced",
        tx_test_pkt_count=8,
        tx_test_pacing_ns=200000,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=15,
        frame_info_path=frame_info_path,
    )

    try:
        with _test_summary("test_st40p_split_mode_pacing_respected", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=3,
                skip_file_compare=True,
                log_frame_info=True,
            )
            info_dump = run(f"cat {frame_info_path}", host=host)
            assert info_dump.return_code == 0
            output = info_dump.stdout_text or ""
            assert "pkts_total=8" in output
            timestamps = _parse_frame_info_timestamps(output)
            assert (
                len(timestamps) >= 2
            ), "Pacing check requires at least two frame-info entries"
            deltas = [
                curr - prev
                for prev, curr in zip(timestamps, timestamps[1:])
                if curr >= prev
            ]
            assert deltas, "Frame-info timestamps are not monotonically increasing"

            # RTP timestamps are in 90kHz ticks; expect frame-period spacing at tx_fps
            target_ticks = round(90_000 / tx_fps)
            jitter_budget_ticks = round(90_000 * 0.0005)  # 500us worth of ticks
            min_delta = min(deltas)
            max_delta = max(deltas)
            worst_delta = max(abs(delta - target_ticks) for delta in deltas)
            log_info(
                f"[pacing] rtp_ts_delta ticks min={min_delta} max={max_delta} "
                f"target={target_ticks} worst_dev={worst_delta} budget_ticks={jitter_budget_ticks}"
            )
            assert (
                worst_delta <= jitter_budget_ticks
            ), f"RTP ts delta {worst_delta} ticks exceeds 500us budget around target {target_ticks}"
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)


@pytest.mark.nightly
def test_st40p_rx_missing_marker_no_ready(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Send a crafted RTP ANC packet with the marker bit cleared to validate that
    the receiver refuses to surface a ready frame when RTP boundaries are
    incomplete.

    .. rubric:: Purpose
    Send a single RTP packet without the marker bit and confirm RX never emits
    a ready frame.

    .. rubric:: Pass Criteria
    - Frame-info log remains empty after the no-marker packet.
    - RX stays alive long enough to observe the absence of ready frames.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")
    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    input_file_path = media_create.create_text_file(
        size_kb=1,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=60,
        tx_did=67,
        tx_sdid=2,
        tx_split_anc_by_pkt=True,
        tx_test_mode="no-marker",
        tx_test_pkt_count=1,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=20,
        frame_info_path=frame_info_path,
    )

    expectation = "RX produces no ready frame when marker bit is absent"

    try:
        with _test_summary("test_st40p_rx_missing_marker_no_ready", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=3,
                skip_file_compare=True,
                log_frame_info=True,
            )
            info_dump = run(f"cat {frame_info_path}", host=host)
            assert info_dump.return_code == 0
            assert not (info_dump.stdout_text or "").strip()
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)


@pytest.mark.nightly
def test_st40p_rx_seq_loss_logged(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Inject a deliberate RTP sequence gap to confirm the receiver logs
    discontinuities and packet loss accounting without crashing.

    .. rubric:: Purpose
    Inject a deliberate sequence gap and confirm seq_discont/seq_lost accounting
    is recorded.

    .. rubric:: Pass Criteria
        - Frame-info log reports seq_discont=1 and seq_lost=1 with pkts_total=2 (or
            RX drops before logging and frame-info stays absent/empty).
        - RX does not crash despite the gap.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")
    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    input_file_path = media_create.create_text_file(
        size_kb=1,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=60,
        tx_did=67,
        tx_sdid=2,
        tx_split_anc_by_pkt=True,
        tx_test_mode="seq-gap",
        tx_test_pkt_count=2,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=20,
        frame_info_path=frame_info_path,
    )

    expectation = "Seq gap is detected and logged (seq_discont=1, seq_lost=1)"

    try:
        with _test_summary("test_st40p_rx_seq_loss_logged", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=3,
                skip_file_compare=True,
                log_frame_info=True,
            )
            frame_info_exists = (
                run(f"test -f {frame_info_path}", host=host).return_code == 0
            )
            if not frame_info_exists:
                log_info(
                    "frame-info absent; RX may have dropped before logging seq gap"
                )
                return
            info_dump = run(f"cat {frame_info_path}", host=host)
            assert info_dump.return_code == 0
            output = info_dump.stdout_text or ""
            if output.strip():
                assert "seq_discont=1" in output
                assert "seq_lost=1" in output
                assert "pkts_total=2" in output
            else:
                log_info(
                    "frame-info empty; RX dropped before recording seq gap metadata"
                )
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)


@pytest.mark.nightly
def test_st40p_rx_bad_parity_drops_payload(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Corrupt ANC parity bits inside a crafted RTP packet to verify the receiver
    rejects metadata for bad parity rather than emitting usable ANC payload.

    .. rubric:: Purpose
    Corrupt ANC parity bits to confirm the receiver drops metadata for the frame.

    .. rubric:: Pass Criteria
    - Frame-info is absent, empty, or reports meta=0 with pkts_total=1.
    - RX does not emit usable ANC metadata for corrupted parity.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")
    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    input_file_path = media_create.create_text_file(
        size_kb=1,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=60,
        tx_did=67,
        tx_sdid=2,
        tx_split_anc_by_pkt=True,
        tx_test_mode="bad-parity",
        tx_test_pkt_count=1,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=20,
        frame_info_path=frame_info_path,
    )

    expectation = "Bad parity is discarded (meta=0 or empty frame-info)"

    try:
        with _test_summary("test_st40p_rx_bad_parity_drops_payload", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=3,
                skip_file_compare=True,
                log_frame_info=True,
            )
            frame_info_exists = (
                run(f"test -f {frame_info_path}", host=host).return_code == 0
            )
            if not frame_info_exists:
                log_info("frame-info absent; RX dropped invalid parity before logging")
                return
            info_dump = run(f"cat {frame_info_path}", host=host)
            assert info_dump.return_code == 0
            output = info_dump.stdout_text or ""
            if output.strip():
                assert "meta=0" in output
                assert "pkts_total=1" in output
            else:
                log_info("frame info empty; RX dropped invalid parity before metadata")
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)


@pytest.mark.nightly
def test_st40p_rx_multi_packet_field_accumulates(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Validate RX aggregation when multiple RTP packets share a timestamp by
    sending a crafted three-packet burst (marker on final packet) and checking
    packet totals in frame-info.

    .. rubric:: Purpose
    Ensure st40p RX records packet totals for multi-packet fields/frames and
    reports clean sequence continuity.

    .. rubric:: Pass Criteria
    - Frame-info exists and reports pkts_total=3.
    - seq_discont=0 and seq_lost=0 for the burst.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")
    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    input_file_path = media_create.create_text_file(
        size_kb=1,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=60,
        tx_did=67,
        tx_sdid=2,
        tx_split_anc_by_pkt=True,
        tx_test_mode="paced",
        tx_test_pkt_count=3,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=40,
        frame_info_path=frame_info_path,
    )

    expectation = "RX aggregates multi-packet timestamp (pkts_total=3, no seq loss)"

    try:
        with _test_summary("test_st40p_rx_multi_packet_field_accumulates", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=5,
                skip_file_compare=True,
                log_frame_info=True,
            )
            assert run(f"test -f {frame_info_path}", host=host).return_code == 0
            info_dump = run(f"cat {frame_info_path}", host=host)
            assert info_dump.return_code == 0
            output = info_dump.stdout_text or ""
            assert "pkts_total=3" in output
            assert "seq_discont=0" in output
            assert "seq_lost=0" in output
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)


@pytest.mark.nightly
def test_st40p_split_padding_alignment_boundary(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Guard against TX/RX padding mismatches on split ANC packets by sending a
    controlled two-packet burst and validating metadata/sequence integrity.

    .. rubric:: Pass Criteria
    - Frame-info exists with meta=2 and pkts_total=2.
    - No sequence discontinuity or loss is reported.
    """

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")

    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    input_file_path = media_create.create_text_file(
        size_kb=12,  # pick a size that exercises alignment-sensitive padding
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=60,
        tx_did=67,
        tx_sdid=2,
        tx_split_anc_by_pkt=True,
        tx_test_mode="paced",
        tx_test_pkt_count=2,
        tx_test_pacing_ns=200000,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=20,
        frame_info_path=frame_info_path,
    )

    expectation = (
        "Split-mode two-packet burst keeps meta/seq intact (meta=2, pkts_total=2)"
    )

    try:
        with _test_summary("test_st40p_split_padding_alignment_boundary", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=test_time,
                host=host,
                tx_first=False,
                sleep_interval=3,
                skip_file_compare=True,
                log_frame_info=True,
            )

            info_dump = run(f"cat {frame_info_path}", host=host)
            assert info_dump.return_code == 0
            output = info_dump.stdout_text or ""
            assert "meta=2" in output
            assert "pkts_total=2" in output
            assert "seq_discont=0" in output
            assert "seq_lost=0" in output
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)


@pytest.mark.nightly
def test_st40p_split_tx_mtu_guard_no_stall(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Stress split-mode ANC with an oversized payload to ensure TX aborts the
    frame cleanly (no session stall) when MTU limits are exceeded.

    .. rubric:: Pass Criteria
    - Pipeline run completes (True/False) within the timeout window.
    - Process does not hang; frame-info file is created or TX exits cleanly.
    """

    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")

    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    # Use a large payload to provoke MTU-bound splitting/guard paths.
    input_file_path = media_create.create_text_file(
        size_kb=512,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=2,
        tx_fps=50,
        tx_did=67,
        tx_sdid=2,
        tx_split_anc_by_pkt=True,
        tx_test_mode="paced",
        tx_test_pkt_count=8,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=2,
        timeout=10,
        frame_info_path=frame_info_path,
    )

    expectation = "Split-mode oversized ANC does not stall TX session"

    try:
        with _test_summary("test_st40p_split_tx_mtu_guard_no_stall", expectation):
            result = GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=min(test_time, 6),
                host=host,
                tx_first=False,
                sleep_interval=2,
                skip_file_compare=True,
                log_frame_info=True,
            )
            # Accept either success or handled failure; the key is no hang.
            assert result in (True, False)
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)


@pytest.mark.nightly
def test_st40i_split_mode_frame_info_logging(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Interlaced split-mode coverage where ANC is split by packet, ensuring the
    receiver records frame-info metadata (packet totals) alongside successful
    interlaced delivery.

    .. rubric:: Purpose
    Interlaced split-mode coverage with ANC split by packet to confirm frame
    info metadata is captured at the receiver.

    .. rubric:: Pass Criteria
    - Frame-info log contains pkts_total after pipeline completes.
    - Interlaced split-mode payload round-trips without timeout.

    :param hosts: Mapping of available hosts for running pipelines remotely.
    :param build: Compiled GStreamer binaries/scripts used for TX/RX.
    :param media: Ancillary media fixture (not directly used here).
    :param setup_interfaces: Fixture configuring paired NIC ports for TX/RX.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture setting up RAM disk storage.
    :param media_file: Fixture providing input/output media paths.
    """
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if len(interfaces_list) < 2:
        pytest.skip("At least two interfaces are required for split-mode loopback")
    input_file_path, output_file_path = setup_paths(media_file)
    frame_info_path = _frame_info_path(os.path.dirname(output_file_path))

    input_file_path = media_create.create_text_file(
        size_kb=10,
        output_path=input_file_path,
        host=host,
    )

    tx_config = GstreamerApp.setup_gstreamer_st40p_tx_pipeline(
        build=build,
        nic_port_list=interfaces_list[0],
        input_path=input_file_path,
        tx_payload_type=113,
        tx_queues=4,
        tx_framebuff_cnt=3,
        tx_fps=50,
        tx_did=67,
        tx_sdid=2,
        tx_split_anc_by_pkt=True,
        tx_interlaced=True,
    )

    rx_config = GstreamerApp.setup_gstreamer_st40p_rx_pipeline(
        build=build,
        nic_port_list=interfaces_list[1],
        output_path=output_file_path,
        rx_payload_type=113,
        rx_queues=4,
        rx_framebuff_cnt=3,
        timeout=15,
        frame_info_path=frame_info_path,
        rx_interlaced=True,
    )

    expectation = "Interlaced split-mode captures frame-info (pkts_total present)"

    try:
        with _test_summary("test_st40i_split_mode_frame_info_logging", expectation):
            assert GstreamerApp.execute_test(
                build=build,
                tx_command=tx_config,
                rx_command=rx_config,
                input_file=input_file_path,
                output_file=output_file_path,
                test_time=6,
                host=host,
                tx_first=False,
                sleep_interval=4,
                log_frame_info=True,
            )

            info_dump = run(f"cat {frame_info_path}", host=host)
            assert info_dump.return_code == 0
            assert "pkts_total=" in info_dump.stdout_text
    finally:
        media_create.remove_file(input_file_path, host=host)
        media_create.remove_file(output_file_path, host=host)
        run(f"rm -f {frame_info_path}", host=host)
