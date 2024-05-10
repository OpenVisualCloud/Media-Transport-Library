# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

"""Module of MTL st20p rx encode python example."""

import ctypes
import sys

import av
import misc_util
import numpy as np
import pymtl as mtl


def main():
    args = misc_util.parse_args(False)

    mtl_output_fmt = mtl.ST_FRAME_FMT_YUV420PLANAR8
    av_pixel_input_format = "yuv420p"

    output_file = args.rx_url

    # Init para
    init_para = mtl.mtl_init_params()
    mtl.mtl_para_port_set(init_para, mtl.MTL_PORT_P, args.p_port)
    mtl.mtl_para_pmd_set(
        init_para, mtl.MTL_PORT_P, mtl.mtl_pmd_by_port_name(args.p_port)
    )
    init_para.num_ports = 1
    mtl.mtl_para_sip_set(init_para, mtl.MTL_PORT_P, args.p_sip)
    init_para.flags = mtl.MTL_FLAG_BIND_NUMA | mtl.MTL_FLAG_DEV_AUTO_START_STOP
    if args.ptp:
        init_para.flags |= mtl.MTL_FLAG_PTP_ENABLE
    mtl.mtl_para_tx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 0)
    mtl.mtl_para_rx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 1)
    init_para.rss_mode = args.rss_mode
    init_para.pacing = args.pacing_way
    init_para.nb_tx_desc = args.nb_tx_desc
    init_para.nb_rx_desc = args.nb_rx_desc
    if args.lcores:
        init_para.lcores = args.lcores

    # Create MTL instance
    mtl_handle = mtl.mtl_init(init_para)
    if not mtl_handle:
        print("mtl_init fail")
        sys.exit(1)

    # Create st20p rx session
    rx_para = mtl.st20p_rx_ops()
    rx_para.name = "st20p_rx_python"
    rx_para.width = args.width
    rx_para.height = args.height
    rx_para.fps = args.fps
    rx_para.framebuff_cnt = 3
    rx_para.transport_fmt = args.transport_fmt
    rx_para.output_fmt = mtl_output_fmt
    # rx port
    rx_port = mtl.st_rx_port()
    mtl.st_rxp_para_port_set(
        rx_port,
        mtl.MTL_SESSION_PORT_P,
        mtl.mtl_para_port_get(init_para, mtl.MTL_SESSION_PORT_P),
    )
    rx_port.num_port = 1
    mtl.st_rxp_para_ip_set(rx_port, mtl.MTL_SESSION_PORT_P, args.p_rx_ip)
    mtl.st_rxp_para_udp_port_set(rx_port, mtl.MTL_SESSION_PORT_P, args.udp_port)
    rx_port.payload_type = args.payload_type
    rx_para.port = rx_port
    # enable block get mode
    rx_para.flags = mtl.ST20P_RX_FLAG_BLOCK_GET
    # create st20p_rx session
    st20p_rx = mtl.st20p_rx_create(mtl_handle, rx_para)
    if not st20p_rx:
        print("st20p_rx_create fail")
        sys.exit(1)

    # create h264_stream
    h264 = av.open(output_file, mode="w")
    h264_stream = h264.add_stream("libx264", rate=60)
    h264_stream.width = args.width
    h264_stream.height = args.height
    h264_stream.pix_fmt = "yuv420p"  # h264 use yuv420p

    # loop until ctrl-c
    try:
        while True:
            frame = mtl.st20p_rx_get_frame(st20p_rx)
            if not frame:
                continue

            video_frame = av.VideoFrame(args.width, args.height, av_pixel_input_format)
            for plane in range(mtl.st_frame_fmt_planes(frame.fmt)):
                p_size = mtl.st_frame_plane_size(frame, plane)
                # print(f"plane: {plane} size: {p_size}")
                ptr = (ctypes.c_ubyte * p_size).from_address(
                    mtl.st_frame_addr_cpuva(frame, plane)
                )
                p = np.ctypeslib.as_array(ptr, (p_size,))
                video_frame.planes[plane].update(p)

            for packet in h264_stream.encode(video_frame):
                h264.mux(packet)

            # return the frame
            mtl.st20p_rx_put_frame(st20p_rx, frame)

    except KeyboardInterrupt:
        print("KeyboardInterrupt")

    # write eof stream packet
    for packet in h264_stream.encode():
        h264.mux(packet)

    # Free st20p_rx session
    mtl.st20p_rx_free(st20p_rx)

    # Free MTL instance
    mtl.mtl_uninit(mtl_handle)

    print(f"Everything fine with {output_file}, bye")


if __name__ == "__main__":
    main()
