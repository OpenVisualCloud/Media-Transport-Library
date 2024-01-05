# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation
# misc utils

import argparse
import ctypes
import sys

import cv2
import numpy as np
import pymtl as mtl


def parse_pipeline_fmt(name):
    fmt = mtl.st_frame_name_to_fmt(name)
    if fmt >= mtl.ST_FRAME_FMT_MAX:
        raise argparse.ArgumentTypeError(f"{name} is not a valid fmt name")
    return fmt


def parse_st22_codec(name):
    if name == "jpegxs":
        return mtl.ST22_CODEC_JPEGXS
    if name == "h264_cbr":
        return mtl.ST22_CODEC_H264_CBR

    raise argparse.ArgumentTypeError(f"{name} is not a valid codec name")


def parse_args(is_tx):
    parser = argparse.ArgumentParser(description="Argument util for MTL example")
    if is_tx:
        p_port_default = "0000:af:01.1"
    else:
        p_port_default = "0000:af:01.0"
    parser.add_argument(
        "--p_port", type=str, default=p_port_default, help="primary port name"
    )
    if is_tx:
        p_sip_default = "192.168.108.101"
    else:
        p_sip_default = "192.168.108.102"
    parser.add_argument(
        "--p_sip", type=str, default=p_sip_default, help="primary local IP address"
    )
    # p_tx_ip
    parser.add_argument(
        "--p_tx_ip",
        type=str,
        default="239.168.85.20",
        help="primary TX dest IP address",
    )
    # p_rx_ip
    parser.add_argument(
        "--p_rx_ip",
        type=str,
        default="239.168.85.20",
        help="primary RX source IP address",
    )
    # pipeline_fmt
    parser.add_argument(
        "--pipeline_fmt",
        type=parse_pipeline_fmt,
        default=mtl.ST_FRAME_FMT_YUV422PLANAR10LE,
        help="pipeline_fmt",
    )
    # display
    parser.add_argument("--display", action="store_true", help="enable display option")
    parser.add_argument(
        "--display_scale_factor", type=int, default=2, help="display scale factor"
    )
    # tx_url
    parser.add_argument(
        "--tx_url", type=str, default="yuv422p10le_1080p.yuv", help="tx url file path"
    )
    # rx_url
    parser.add_argument(
        "--rx_url", type=str, default="test.mp4", help="rx url file path"
    )
    # width & height
    parser.add_argument("--width", type=int, default=1920, help="width")
    parser.add_argument("--height", type=int, default=1080, help="height")
    # interlaced
    parser.add_argument(
        "--interlaced", action="store_true", help="Enable interlaced option"
    )
    # udp_port
    parser.add_argument("--udp_port", type=int, default=20000, help="udp port")
    # payload_type
    parser.add_argument("--payload_type", type=int, default=112, help="payload type")
    # ptp
    parser.add_argument("--ptp", action="store_true", help="Enable built-in PTP option")
    # st22_codec
    parser.add_argument(
        "--st22_codec",
        type=parse_st22_codec,
        default=mtl.ST22_CODEC_JPEGXS,
        help="st22_codec",
    )
    return parser.parse_args()


def ptr_to_yuv422p8(ptr, width, height):
    y_size = width * height
    u_size = width * height // 2

    frame = np.frombuffer(ptr, dtype=np.uint8)

    y = frame[:y_size].reshape((height, width))
    u = frame[y_size : y_size + u_size].reshape((height, width // 2))
    v = frame[y_size + u_size :].reshape((height, width // 2))

    return y, u, v


def yuv422p10le_to_yuv422(ptr, width, height):
    y_size = width * height
    u_size = width * height // 2

    frame = np.frombuffer(ptr, dtype=np.uint16)

    y = frame[:y_size].reshape((height, width))
    u = frame[y_size : y_size + u_size].reshape((height, width // 2))
    v = frame[y_size + u_size :].reshape((height, width // 2))

    y = np.right_shift(y, 2).astype(np.uint8)
    u = np.right_shift(u, 2).astype(np.uint8)
    v = np.right_shift(v, 2).astype(np.uint8)

    return y, u, v


def downscale_yuv422(y, u, v, scale_factor):
    height, width = y.shape

    y_downscaled = cv2.resize(
        y,
        (width // scale_factor, height // scale_factor),
        interpolation=cv2.INTER_LINEAR,
    )
    u_downscaled = cv2.resize(
        u,
        (width // (2 * scale_factor), height // scale_factor),
        interpolation=cv2.INTER_LINEAR,
    )
    v_downscaled = cv2.resize(
        v,
        (width // (2 * scale_factor), height // scale_factor),
        interpolation=cv2.INTER_LINEAR,
    )

    return y_downscaled, u_downscaled, v_downscaled


def display_yuv422(y, u, v):
    u = cv2.resize(u, (y.shape[1], y.shape[0]), interpolation=cv2.INTER_LINEAR)
    v = cv2.resize(v, (y.shape[1], y.shape[0]), interpolation=cv2.INTER_LINEAR)

    yuv = cv2.merge([y, u, v])
    display_img = cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR)
    cv2.imshow(sys.argv[0], display_img)
    cv2.waitKey(1)


def destroy():
    cv2.destroyAllWindows()


def frame_display_yuv422p8(frame, display_scale_factor):
    # Pack frame pointer from frame addr
    ptr = (ctypes.c_ubyte * (frame.data_size)).from_address(
        mtl.st_frame_addr_cpuva(frame, 0)
    )
    width = frame.width
    height = frame.height

    # Convert to yuv422 planar 8
    y, u, v = ptr_to_yuv422p8(ptr, width, height)
    # Downscale
    y_scale, u_scale, v_scale = downscale_yuv422(y, u, v, display_scale_factor)
    # Convert to RGB and display
    display_yuv422(y_scale, u_scale, v_scale)


def frame_display_yuv422p10le(frame, display_scale_factor):
    # Pack frame pointer from frame addr
    ptr = (ctypes.c_ubyte * (frame.data_size)).from_address(
        mtl.st_frame_addr_cpuva(frame, 0)
    )
    width = frame.width
    height = frame.height

    # It's slow since it include many Converting steps, just demo the usage
    # Convert to yuv422 planar 8
    y, u, v = yuv422p10le_to_yuv422(ptr, width, height)
    # Downscale
    y_scale, u_scale, v_scale = downscale_yuv422(y, u, v, display_scale_factor)
    # Convert to RGB and display
    display_yuv422(y_scale, u_scale, v_scale)


def frame_display_uyvy(frame, display_scale_factor):
    # Pack frame pointer from frame addr
    ptr = (ctypes.c_ubyte * (frame.data_size)).from_address(
        mtl.st_frame_addr_cpuva(frame, 0)
    )
    width = frame.width
    height = frame.height
    downscaled_width = width // display_scale_factor
    downscaled_height = height // display_scale_factor

    uyvy = np.frombuffer(ptr, dtype=np.uint8).reshape((height, width, 2))
    rgb = cv2.cvtColor(uyvy, cv2.COLOR_YUV2BGR_UYVY)
    downscaled_rgb = cv2.resize(
        rgb, (downscaled_width, downscaled_height), interpolation=cv2.INTER_AREA
    )
    cv2.imshow("st20p_rx_python", downscaled_rgb)
    cv2.waitKey(1)


def frame_display_rfc4175be10(mtl_handle, frame, display_scale_factor):
    yuv422p8_frame = mtl.st_frame_create(
        mtl_handle,
        mtl.ST_FRAME_FMT_YUV422PLANAR8,
        frame.width,
        frame.height,
        frame.interlaced,
    )
    if yuv422p8_frame:
        mtl.st_frame_convert(frame, yuv422p8_frame)
        frame_display_yuv422p8(yuv422p8_frame, display_scale_factor)
        mtl.st_frame_free(yuv422p8_frame)


def frame_display(mtl_handle, frame, display_scale_factor):
    if frame.fmt == mtl.ST_FRAME_FMT_YUV422PLANAR10LE:
        frame_display_yuv422p10le(frame, display_scale_factor)
    elif frame.fmt == mtl.ST_FRAME_FMT_UYVY:
        frame_display_uyvy(frame, display_scale_factor)
    elif frame.fmt == mtl.ST_FRAME_FMT_YUV422PLANAR8:
        frame_display_yuv422p8(frame, display_scale_factor)
    elif frame.fmt == mtl.ST_FRAME_FMT_YUV422RFC4175PG2BE10:
        frame_display_rfc4175be10(mtl_handle, frame, display_scale_factor)
    else:
        print(f"Unknown fmt: {mtl.st_frame_fmt_name(frame.fmt)}")


def field_display(mtl_handle, first, second, display_scale_factor):
    frame = mtl.st_frame_create(
        mtl_handle,
        first.fmt,
        first.width,
        first.height,
        False,
    )
    if frame:
        mtl.st_field_merge(first, second, frame)
        frame_display(mtl_handle, frame, display_scale_factor)
        mtl.st_frame_free(frame)


def copy_to_st_frame(yuv_frame, frame):
    src_p = ctypes.c_char_p(yuv_frame)
    src_address = ctypes.cast(src_p, ctypes.c_void_p).value
    src_address_uint64 = ctypes.c_uint64(src_address).value
    # mtl_memcpy_action
    memcpy_ops = mtl.mtl_memcpy_ops()
    memcpy_ops.dst = mtl.st_frame_addr_cpuva(frame, 0)
    memcpy_ops.src = src_address_uint64
    memcpy_ops.sz = frame.data_size
    mtl.mtl_memcpy_action(memcpy_ops)
