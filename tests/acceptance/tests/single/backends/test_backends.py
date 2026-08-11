# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""Transport backends across representative load points.

Each case asserts the resolved port type because MTL can fall back to another
backend without failing initialization.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files

pytestmark = [pytest.mark.verified, pytest.mark.nightly]


@pytest.mark.parametrize("session_type", ["st20p", "st22p"])
@pytest.mark.parametrize(
    "media_file", [yuv_files["i1080p59"]], indirect=True, ids=["i1080p59"]
)
def test_backend_kernel_socket(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    session_type,
    output_files,
    media_integrity,
    media_file,
):
    """A kernel-socket receiver must deliver the stream the PMD sent."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    ports = setup_interfaces.get_pmd_kernel_interfaces("VF")
    app = app_factory("rxtxapp")
    config_params = dict(
        session_type=session_type,
        nic_port_list=ports,
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        pixel_format=media_info["file_format"],
        test_time=test_time,
    )
    if session_type == "st20p":
        config_params["transport_format"] = media_info["format"]
        config_params["output_file"] = output_files.register(f"{media_path}.ksock.out")
    else:
        config_params.update(codec="JPEG-XS", quality="speed", codec_threads=2)
        media_integrity.skip("JPEG-XS is lossy; bytes are not expected to match")
    app.create_command(**config_params)
    run_ok = app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        integrity=media_integrity,
        interface_setup=setup_interfaces,
        fail_on_error=False,
    )
    app.assert_port_types(["vf", "kernel_socket"])
    assert run_ok, "kernel-socket stream failed application or integrity validation"


@pytest.mark.parametrize("test_mode", ["unicast", "multicast"])
@pytest.mark.parametrize("session_type", ["st20p", "st22p"])
@pytest.mark.parametrize(
    "media_file", [yuv_files["i1080p59"]], indirect=True, ids=["i1080p59"]
)
def test_backend_af_xdp(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    session_type,
    test_mode,
    output_files,
    media_integrity,
    media_file,
):
    """AF_XDP has its own send path, so it is a separate transport to prove."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    ports = setup_interfaces.get_native_af_xdp_interfaces()
    # XDP program load and ARP resolution are slower to settle than a VF.
    test_time = max(test_time, 90)
    app = app_factory("rxtxapp")
    config_params = dict(
        session_type=session_type,
        nic_port_list=ports,
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        test_mode=test_mode,
        input_file=media_path,
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        pixel_format=media_info["file_format"],
        test_time=test_time,
    )
    if session_type == "st20p":
        config_params["transport_format"] = media_info["format"]
        config_params["output_file"] = output_files.register(f"{media_path}.xdp.out")
    else:
        config_params.update(codec="JPEG-XS", quality="speed", codec_threads=2)
        media_integrity.skip("JPEG-XS is lossy; bytes are not expected to match")
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, integrity=media_integrity
    )
    app.assert_port_types(["native_af_xdp", "native_af_xdp"])


@pytest.mark.parametrize("afxdp_zc_disable", [False, True], ids=["zc_on", "zc_off"])
@pytest.mark.parametrize(
    "media_file", [yuv_files["i2160p59"]], indirect=True, ids=["i2160p59"]
)
def test_backend_af_xdp_zero_copy(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    afxdp_zc_disable,
    output_files,
    media_integrity,
    media_file,
):
    """Zero-copy changes who owns the buffer, so both settings must deliver."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    ports = setup_interfaces.get_native_af_xdp_interfaces()
    test_time = max(test_time, 90)
    app = app_factory("rxtxapp")
    config_params = dict(
        session_type="st20p",
        nic_port_list=ports,
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.xdpzc.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        test_time=test_time,
        afxdp_zc_disable=afxdp_zc_disable,
    )
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, integrity=media_integrity
    )
    app.assert_port_types(["native_af_xdp", "native_af_xdp"])
    app.assert_af_xdp_zero_copy(not afxdp_zc_disable)


@pytest.mark.parametrize("replicas", [1, 4], ids=["x1", "x4"])
@pytest.mark.parametrize(
    "media_file", [yuv_files["i1080p59"]], indirect=True, ids=["i1080p59"]
)
def test_backend_virtio_user(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    replicas,
    output_files,
    media_integrity,
    media_file,
):
    """virtio-user exposes the port to the kernel without displacing the PMD."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    app = app_factory("rxtxapp")
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single("VF"),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.virtio.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        test_time=test_time,
        virtio_user=True,
        replicas=replicas,
    )
    if replicas > 1:
        config_params.pop("output_file")
        media_integrity.skip("replicated receivers cannot share one integrity output")
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, integrity=media_integrity
    )
    app.assert_port_types(["vf", "vf"])
    app.assert_virtio_user([0, 1])


@pytest.mark.parametrize("pacing_way", ["rl", "tsc"])
@pytest.mark.parametrize(
    "media_file", [yuv_files["i1080p59"]], indirect=True, ids=["i1080p59"]
)
def test_backend_pf(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    pacing_way,
    output_files,
    media_integrity,
    media_file,
):
    """A PF-owned device must use the requested pacing implementation."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    # Both data ports must be PFs, and nicctl refuses to hand out one that
    # shares a card with the capture NIC, so this needs a third interface.
    if len(host.network_interfaces) < 3:
        pytest.skip(
            "PF-to-PF needs two data PFs plus the capture NIC; topology has "
            f"{len(host.network_interfaces)} interface(s)"
        )
    app = app_factory("rxtxapp")
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single("PF", count=2),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.pf.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        test_time=test_time,
        pacing_way=pacing_way,
    )
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path, test_time=test_time, host=host, integrity=media_integrity
    )
    app.assert_port_types(["pf", "pf"])
    app.assert_pacing_way(pacing_way)
