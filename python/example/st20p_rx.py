# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

from pickle import TRUE

import cv2_util
import pymtl as mtl


def main():
    display = TRUE
    display_scale_factor = 2
    # ST_FRAME_FMT_YUV422PLANAR10LE, mtl.ST_FRAME_FMT_UYVY
    # or ST_FRAME_FMT_YUV422RFC4175PG2BE10, ST_FRAME_FMT_YUV422PLANAR8
    output_fmt = mtl.ST_FRAME_FMT_YUV422PLANAR8

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
    rx_para.output_fmt = output_fmt
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
                if display:
                    if output_fmt == mtl.ST_FRAME_FMT_YUV422PLANAR10LE:
                        cv2_util.frame_display_yuv422p10le(frame, display_scale_factor)
                    elif output_fmt == mtl.ST_FRAME_FMT_UYVY:
                        cv2_util.frame_display_uyvy(frame, display_scale_factor)
                    elif output_fmt == mtl.ST_FRAME_FMT_YUV422PLANAR8:
                        cv2_util.frame_display_yuv422p8(frame, display_scale_factor)
                    elif output_fmt == mtl.ST_FRAME_FMT_YUV422RFC4175PG2BE10:
                        cv2_util.frame_display_rfc4175be10(
                            mtl_handle, frame, display_scale_factor
                        )
                    else:
                        print(
                            f"Unknown output_fmt: {mtl.st_frame_fmt_name(output_fmt)}"
                        )
                # return the frame
                mtl.st20p_rx_put_frame(st20p_rx, frame)

    except KeyboardInterrupt:
        print("KeyboardInterrupt")

    cv2_util.destroy()

    # Free st20p_rx session
    mtl.st20p_rx_free(st20p_rx)

    # Free MTL instance
    mtl.mtl_uninit(mtl_handle)

    print("Everything fine, bye")


if __name__ == "__main__":
    main()
