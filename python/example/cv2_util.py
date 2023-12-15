# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation
# opencv2 utils

import ctypes

import cv2
import numpy as np
import pymtl as mtl


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
    cv2.imshow("st20p_rx_python", display_img)
    cv2.waitKey(1)


def destroy():
    cv2.destroyAllWindows()


def frame_display_yuv422p10le(frame, display_scale_factor):
    # Pack frame pointer from frame addr
    ptr = (ctypes.c_ubyte * (frame.data_size)).from_address(mtl.st_frame_addr(frame, 0))
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
    ptr = (ctypes.c_ubyte * (frame.data_size)).from_address(mtl.st_frame_addr(frame, 0))
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
    yuv422p10le_frame = mtl.st_frame_create(
        mtl_handle,
        mtl.ST_FRAME_FMT_YUV422PLANAR10LE,
        frame.width,
        frame.height,
        frame.interlaced,
    )
    if yuv422p10le_frame:
        mtl.st_frame_convert(frame, yuv422p10le_frame)
        frame_display_yuv422p10le(yuv422p10le_frame, display_scale_factor)
        mtl.st_frame_free(yuv422p10le_frame)
