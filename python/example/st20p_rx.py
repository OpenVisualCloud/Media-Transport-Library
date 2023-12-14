# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

import ctypes

import cv2
import numpy as np
import pymtl as mtl


def yuv422p10le_to_yuv422(ptr, width, height):
    y_size = width * height
    u_size = v_size = width * height // 2

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


def main():
    display_scale_factor = 2

    # Init para
    init_para = mtl.mtl_init_params()
    mtl.mtl_para_port_set(init_para, mtl.MTL_PORT_P, "0000:af:01.0")
    init_para.num_ports = 1
    mtl.mtl_para_sip_set(init_para, mtl.MTL_PORT_P, "192.168.108.102")
    init_para.flags = mtl.MTL_FLAG_BIND_NUMA | mtl.MTL_FLAG_DEV_AUTO_START_STOP
    mtl.mtl_para_tx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 0)
    mtl.mtl_para_rx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 1)

    # Create MTL instance
    mtl_handle = mtl.mtl_init(init_para)

    # Create st20p rx session
    rx_para = mtl.st20p_rx_ops()
    rx_para.name = "st20p_rx_python"
    rx_para.width = 1920
    rx_para.height = 1080
    rx_para.fps = mtl.ST_FPS_P59_94
    rx_para.framebuff_cnt = 3
    rx_para.transport_fmt = mtl.ST20_FMT_YUV_422_10BIT
    rx_para.output_fmt = mtl.ST_FRAME_FMT_YUV422PLANAR10LE
    # rx port
    rx_port = mtl.st_rx_port()
    mtl.st_rxp_para_port_set(rx_port, mtl.MTL_SESSION_PORT_P, "0000:af:01.0")
    rx_port.num_port = 1
    mtl.st_rxp_para_sip_set(rx_port, mtl.MTL_SESSION_PORT_P, "239.168.85.20")
    mtl.st_rxp_para_udp_port_set(rx_port, mtl.MTL_SESSION_PORT_P, 20000)
    rx_port.payload_type = 112
    rx_para.port = rx_port
    # create st20p_rx session
    st20p_rx = mtl.st20p_rx_create(mtl_handle, rx_para)

    # loop until ctrl-c
    try:
        while True:
            frame = mtl.st20p_rx_get_frame(st20p_rx)
            if frame:
                # print(f"frame addr: {hex(mtl.st_frame_addr(frame, 0))}")
                # print(f"frame iova: {hex(mtl.st_frame_iova(frame, 0))}")
                # print(f"pkts_total: {frame.pkts_total}")

                # Pack frame pointer from frame addr
                ptr = (ctypes.c_ubyte * (frame.data_size)).from_address(
                    mtl.st_frame_addr(frame, 0)
                )
                width = frame.width
                height = frame.height

                # It's slow since it include many Converting steps, just demo the usage
                # Convert to yuv422 planar 8
                y, u, v = yuv422p10le_to_yuv422(ptr, width, height)
                # Downscale
                y_scale, u_scale, v_scale = downscale_yuv422(
                    y, u, v, display_scale_factor
                )
                # Convert to RGB and display
                display_yuv422(y_scale, u_scale, v_scale)

                # return the frame
                mtl.st20p_rx_put_frame(st20p_rx, frame)

    except KeyboardInterrupt:
        print("KeyboardInterrupt")

    cv2.destroyAllWindows()

    # Free st20p_rx session
    mtl.st20p_rx_free(st20p_rx)

    # Free MTL instance
    mtl.mtl_uninit(mtl_handle)

    print("Everything fine, bye")


if __name__ == "__main__":
    main()
