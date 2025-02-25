# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

"""Module of MTL st20p tx decode python example."""

import ctypes
import sys

import av
import misc_util
import numpy as np
import pymtl as mtl


def process_frame(st20p_tx, frame, av_pixel_output_format):
    # print(f"{frame.format.name}");
    # convert to av_pixel_output_format
    yuv_frame = frame.reformat(format=av_pixel_output_format)
    tx_frame = mtl.st20p_tx_get_frame(st20p_tx)
    if not tx_frame:
        print("st20p_tx_get_frame fail, skip this video frame")
    else:
        width = frame.width
        height = frame.height
        y_size = width * height
        u_size = y_size // 2
        v_size = u_size
        yuv_array = np.empty(y_size + u_size + v_size, dtype=np.uint16)

        # pyav yuv422p10le not support Conversion to numpy array, use copy mode
        yuv_array[:y_size] = np.frombuffer(yuv_frame.planes[0], np.uint16)
        yuv_array[y_size : y_size + u_size] = np.frombuffer(yuv_frame.planes[1], np.uint16)
        yuv_array[y_size + u_size :] = np.frombuffer(yuv_frame.planes[2], np.uint16)
        src_p = ctypes.c_char_p(yuv_array.ctypes.data)
        src_address = ctypes.cast(src_p, ctypes.c_void_p).value
        src_address_uint64 = ctypes.c_uint64(src_address).value

        memcpy_ops = mtl.mtl_memcpy_ops()
        memcpy_ops.dst = mtl.st_frame_addr_cpuva(tx_frame, 0)
        memcpy_ops.src = src_address_uint64
        memcpy_ops.sz = tx_frame.data_size
        mtl.mtl_memcpy_action(memcpy_ops)

        mtl.st20p_tx_put_frame(st20p_tx, tx_frame)


def get_video_resolution(video_path):
    with av.open(video_path) as container:
        video_stream = next(s for s in container.streams if s.type == "video")
        width = video_stream.width
        height = video_stream.height
        return (width, height)


def main():
    args = misc_util.parse_args(True)

    width, height = get_video_resolution(args.tx_url)
    print(f"tx_url: {args.tx_url}, width: {width}, height: {height}")
    mtl_input_fmt = mtl.ST_FRAME_FMT_YUV422PLANAR10LE
    av_pixel_output_format = "yuv422p10le"

    # Init para
    init_para = mtl.mtl_init_params()
    mtl.mtl_para_port_set(init_para, mtl.MTL_PORT_P, args.p_port)
    mtl.mtl_para_pmd_set(init_para, mtl.MTL_PORT_P, mtl.mtl_pmd_by_port_name(args.p_port))
    init_para.num_ports = 1
    mtl.mtl_para_sip_set(init_para, mtl.MTL_PORT_P, args.p_sip)
    init_para.flags = mtl.MTL_FLAG_BIND_NUMA | mtl.MTL_FLAG_DEV_AUTO_START_STOP
    if args.ptp:
        init_para.flags |= mtl.MTL_FLAG_PTP_ENABLE
    mtl.mtl_para_tx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 1)
    mtl.mtl_para_rx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 0)
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

    # Create st20p tx session
    tx_para = mtl.st20p_tx_ops()
    tx_para.name = "st20p_tx_python"
    tx_para.width = width
    tx_para.height = height
    tx_para.fps = args.fps
    tx_para.framebuff_cnt = 3
    tx_para.transport_fmt = args.transport_fmt
    tx_para.transport_packing = args.packing
    tx_para.input_fmt = mtl_input_fmt
    # tx port
    tx_port = mtl.st_tx_port()
    mtl.st_txp_para_port_set(
        tx_port,
        mtl.MTL_SESSION_PORT_P,
        mtl.mtl_para_port_get(init_para, mtl.MTL_SESSION_PORT_P),
    )
    tx_port.num_port = 1
    mtl.st_txp_para_dip_set(tx_port, mtl.MTL_SESSION_PORT_P, args.p_tx_ip)
    mtl.st_txp_para_udp_port_set(tx_port, mtl.MTL_SESSION_PORT_P, args.udp_port)
    tx_port.payload_type = args.payload_type
    tx_para.port = tx_port
    # enable block get mode
    tx_para.flags = mtl.ST20P_TX_FLAG_BLOCK_GET
    # create st20p_tx session
    st20p_tx = mtl.st20p_tx_create(mtl_handle, tx_para)
    if not st20p_tx:
        print("st20p_tx_create fail")
        sys.exit(1)
    frame_sz = mtl.st20p_tx_frame_size(st20p_tx)
    print(f"frame_sz: {hex(frame_sz)}")

    # loop until ctrl-c
    container = av.open(args.tx_url)
    try:
        stream = next(s for s in container.streams if s.type == "video")
        while True:
            for frame in container.decode(stream):
                process_frame(st20p_tx, frame, av_pixel_output_format)

            # seek to the first frame
            print("Finish play, seek to first frame")
            container.seek(0, stream=stream)

    except KeyboardInterrupt:
        print("KeyboardInterrupt")

    finally:
        container.close()

    # Free st20p_tx session
    mtl.st20p_tx_free(st20p_tx)

    # Free MTL instance
    mtl.mtl_uninit(mtl_handle)

    print("Everything fine, bye")


if __name__ == "__main__":
    main()
