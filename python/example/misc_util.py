# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation
# misc utils

import argparse
import ctypes
import datetime
import sys

import cv2
import numpy as np
import pymtl as mtl


def parse_pipeline_fmt(name):
    fmt = mtl.st_frame_name_to_fmt(name)
    if fmt >= mtl.ST_FRAME_FMT_MAX:
        raise argparse.ArgumentTypeError(f"{name} is not a valid pipeline fmt")
    return fmt


def parse_transport_fmt(name):
    fmt = mtl.st20_name_to_fmt(name)
    if fmt >= mtl.ST20_FMT_MAX:
        raise argparse.ArgumentTypeError(f"{name} is not a valid transport fmt")
    return fmt


def parse_packing(name):
    if name == "bpm":
        return mtl.ST20_PACKING_BPM
    if name == "gpm":
        return mtl.ST20_PACKING_GPM
    if name == "gpm_sl":
        return mtl.ST20_PACKING_GPM_SL

    raise argparse.ArgumentTypeError(f"{name} is not a packing name")


def parse_st22_codec(name):
    if name == "jpegxs":
        return mtl.ST22_CODEC_JPEGXS
    if name == "h264_cbr":
        return mtl.ST22_CODEC_H264_CBR

    raise argparse.ArgumentTypeError(f"{name} is not a valid codec name")


def parse_pacing_way(name):
    if name == "auto":
        return mtl.ST21_TX_PACING_WAY_AUTO
    if name == "rl":
        return mtl.ST21_TX_PACING_WAY_RL
    if name == "tsn":
        return mtl.ST21_TX_PACING_WAY_TSN
    if name == "tsc":
        return mtl.ST21_TX_PACING_WAY_TSC
    if name == "tsc_narrow":
        return mtl.ST21_TX_PACING_WAY_TSC_NARROW
    if name == "ptp":
        return mtl.ST21_TX_PACING_WAY_PTP
    if name == "be":
        return mtl.ST21_TX_PACING_WAY_BE

    raise argparse.ArgumentTypeError(f"{name} is not a valid pacing way")


def parse_rss_mode(name):
    if name == "none":
        return mtl.MTL_RSS_MODE_NONE
    if name == "l3_l4":
        return mtl.MTL_RSS_MODE_L3_L4
    if name == "l3":
        return mtl.MTL_RSS_MODE_L3

    raise argparse.ArgumentTypeError(f"{name} is not a valid rss mode")


def parse_fps(name):
    fps = mtl.st_name_to_fps(name)
    if fps >= mtl.ST_FPS_MAX:
        raise argparse.ArgumentTypeError(f"{name} is not a valid fps name")
    return fps


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
    # nb_tx_desc & nb_rx_desc
    parser.add_argument("--nb_tx_desc", type=int, default=0, help="nb_tx_desc")
    parser.add_argument("--nb_rx_desc", type=int, default=0, help="nb_rx_desc")
    parser.add_argument("--rx_burst_size", type=int, default=0, help="rx_burst_size")
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
    # transport_fmt
    parser.add_argument(
        "--transport_fmt",
        type=parse_transport_fmt,
        default=mtl.ST20_FMT_YUV_422_10BIT,
        help="transport_fmt",
    )
    # packing
    parser.add_argument(
        "--packing",
        type=parse_packing,
        default=mtl.ST20_PACKING_BPM,
        help="packing",
    )
    # fps
    parser.add_argument(
        "--fps",
        type=parse_fps,
        default=mtl.ST_FPS_P59_94,
        help="fps",
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
    # pacing_way
    parser.add_argument(
        "--pacing_way",
        type=parse_pacing_way,
        default=mtl.ST21_TX_PACING_WAY_AUTO,
        help="pacing_way",
    )
    # rss_mode
    parser.add_argument(
        "--rss_mode",
        type=parse_rss_mode,
        default=mtl.MTL_RSS_MODE_NONE,
        help="rss_mode",
    )
    # lcores
    parser.add_argument("--lcores", type=str, default="", help="lcores")
    # log file
    parser.add_argument("--log_file", type=str, default="py_temp.log", help="log_file")
    # default max histories 10 mins, 1 entry per second
    parser.add_argument("--histories", type=int, default=60 * 10, help="histories")
    return parser.parse_args()


def ptr_to_yuv422p8(ptr, width, height):
    y_size = width * height
    u_size = width * height // 2

    frame = np.frombuffer(ptr, dtype=np.uint8)

    y = frame[:y_size].reshape((height, width))
    u = frame[y_size : y_size + u_size].reshape((height, width // 2))
    v = frame[y_size + u_size :].reshape((height, width // 2))

    return y, u, v


def ptr_to_yuv420p8(ptr, width, height):
    y_size = width * height
    u_size = width * height // 4

    frame = np.frombuffer(ptr, dtype=np.uint8)

    y = frame[:y_size].reshape((height, width))
    u = frame[y_size : y_size + u_size].reshape((height // 2, width // 2))
    v = frame[y_size + u_size :].reshape((height // 2, width // 2))

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


def downscale_yuv420(y, u, v, scale_factor):
    height, width = y.shape

    y_downscaled = cv2.resize(
        y,
        (width // scale_factor, height // scale_factor),
        interpolation=cv2.INTER_LINEAR,
    )
    u_downscaled = cv2.resize(
        u,
        (width // (2 * scale_factor), height // (2 * scale_factor)),
        interpolation=cv2.INTER_LINEAR,
    )
    v_downscaled = cv2.resize(
        v,
        (width // (2 * scale_factor), height // (2 * scale_factor)),
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


def display_yuv420(y, u, v):
    u = cv2.resize(u, (y.shape[1], y.shape[0]), interpolation=cv2.INTER_LINEAR)
    v = cv2.resize(v, (y.shape[1], y.shape[0]), interpolation=cv2.INTER_LINEAR)

    yuv = cv2.merge([y, u, v])

    # display_img = cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_I420)
    display_img = cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR)
    cv2.imshow(sys.argv[0], display_img)
    cv2.waitKey(1)


def destroy():
    cv2.destroyAllWindows()


def waitKey():
    cv2.waitKey()


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


def frame_display_yuv420p8(frame, display_scale_factor):
    # Pack frame pointer from frame addr
    ptr = (ctypes.c_ubyte * (frame.data_size)).from_address(
        mtl.st_frame_addr_cpuva(frame, 0)
    )
    width = frame.width
    height = frame.height

    # Convert to yuv420 planar 8
    y, u, v = ptr_to_yuv420p8(ptr, width, height)
    # Downscale
    y_scale, u_scale, v_scale = downscale_yuv420(y, u, v, display_scale_factor)
    # Convert to RGB and display
    display_yuv420(y_scale, u_scale, v_scale)


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


def frame_display_rfc4175be10(frame, display_scale_factor):
    yuv422p8_frame = mtl.st_frame_create_by_malloc(
        mtl.ST_FRAME_FMT_YUV422PLANAR8,
        frame.width,
        frame.height,
        frame.interlaced,
    )
    if yuv422p8_frame:
        mtl.st_frame_convert(frame, yuv422p8_frame)
        frame_display_yuv422p8(yuv422p8_frame, display_scale_factor)
        mtl.st_frame_free(yuv422p8_frame)


def frame_display_v210(frame, display_scale_factor):
    rfc4175be10_frame = mtl.st_frame_create_by_malloc(
        mtl.ST_FRAME_FMT_YUV422RFC4175PG2BE10,
        frame.width,
        frame.height,
        frame.interlaced,
    )
    if rfc4175be10_frame:
        # It's very slow since it include many converting steps, just demo the usage
        mtl.st_frame_convert(frame, rfc4175be10_frame)
        frame_display_rfc4175be10(rfc4175be10_frame, display_scale_factor)
        mtl.st_frame_free(rfc4175be10_frame)


def frame_display(frame, display_scale_factor):
    if frame.fmt == mtl.ST_FRAME_FMT_YUV422PLANAR10LE:
        frame_display_yuv422p10le(frame, display_scale_factor)
    elif frame.fmt == mtl.ST_FRAME_FMT_UYVY:
        frame_display_uyvy(frame, display_scale_factor)
    elif frame.fmt == mtl.ST_FRAME_FMT_YUV422PLANAR8:
        frame_display_yuv422p8(frame, display_scale_factor)
    elif frame.fmt == mtl.ST_FRAME_FMT_YUV422RFC4175PG2BE10:
        frame_display_rfc4175be10(frame, display_scale_factor)
    elif frame.fmt == mtl.ST_FRAME_FMT_YUV420PLANAR8:
        frame_display_yuv420p8(frame, display_scale_factor)
    elif frame.fmt == mtl.ST_FRAME_FMT_V210:
        frame_display_v210(frame, display_scale_factor)
    else:
        print(f"Unknown fmt: {mtl.st_frame_fmt_name(frame.fmt)}")


def field_display(first, second, display_scale_factor):
    frame = mtl.st_frame_create_by_malloc(
        first.fmt,
        first.width,
        first.height,
        False,
    )
    if frame:
        mtl.st_field_merge(first, second, frame)
        frame_display(frame, display_scale_factor)
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


def cur_time_str():
    current_time = datetime.datetime.now()
    formatted_time = current_time.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    return formatted_time
