# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

import sys

import misc_util
import pymtl as mtl


def interlaced_rx_loop(st22p_rx, display, display_scale_factor):
    first = None
    # loop until ctrl-c
    try:
        while True:
            field = mtl.st22p_rx_get_frame(st22p_rx)
            if not field:
                continue
            if field.second_field:
                if first:  # first field get also
                    # print("Both field get")
                    if display:
                        misc_util.field_display(first, field, display_scale_factor)
                    mtl.st22p_rx_put_frame(st22p_rx, first)
                    first = None

                # put field
                mtl.st22p_rx_put_frame(st22p_rx, field)
            else:  # first field
                if first:
                    mtl.st22p_rx_put_frame(st22p_rx, first)
                    first = None
                first = field

    except KeyboardInterrupt:
        print("KeyboardInterrupt")
        if first:
            mtl.st22p_rx_put_frame(st22p_rx, first)
            first = None


def frame_rx_loop(st22p_rx, display, display_scale_factor):
    # loop until ctrl-c
    try:
        while True:
            frame = mtl.st22p_rx_get_frame(st22p_rx)
            if frame:
                # print(f"frame addr: {hex(mtl.st_frame_addr_cpuva(frame, 0))}")
                # print(f"frame iova: {hex(mtl.st_frame_iova(frame, 0))}")
                # print(f"pkts_total: {frame.pkts_total}")
                if display:
                    misc_util.frame_display(frame, display_scale_factor)
                # return the frame
                mtl.st22p_rx_put_frame(st22p_rx, frame)

    except KeyboardInterrupt:
        print("KeyboardInterrupt")


def main():
    args = misc_util.parse_args(False)

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

    # Create st22p rx session
    rx_para = mtl.st22p_rx_ops()
    rx_para.name = "st22p_rx_python"
    rx_para.width = args.width
    rx_para.height = args.height
    rx_para.fps = args.fps
    rx_para.interlaced = args.interlaced
    rx_para.framebuff_cnt = 3
    rx_para.output_fmt = args.pipeline_fmt
    rx_para.pack_type = mtl.ST22_PACK_CODESTREAM
    rx_para.codec = mtl.ST22_CODEC_JPEGXS
    rx_para.device = mtl.ST_PLUGIN_DEVICE_AUTO
    # let lib to decide
    rx_para.max_codestream_size = 0
    rx_para.codec_thread_cnt = 2
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
    rx_para.flags = mtl.ST22P_RX_FLAG_BLOCK_GET
    # create st22p_rx session
    st22p_rx = mtl.st22p_rx_create(mtl_handle, rx_para)
    if not st22p_rx:
        print("st22p_rx_create fail")
        sys.exit(1)

    if args.interlaced:
        interlaced_rx_loop(st22p_rx, args.display, args.display_scale_factor)
    else:
        frame_rx_loop(st22p_rx, args.display, args.display_scale_factor)

    misc_util.destroy()

    # Free st22p_rx session
    mtl.st22p_rx_free(st22p_rx)

    # Free MTL instance
    mtl.mtl_uninit(mtl_handle)

    print("Everything fine, bye")


if __name__ == "__main__":
    main()
