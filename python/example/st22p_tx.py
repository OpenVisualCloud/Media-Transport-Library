# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

import sys

import misc_util
import pymtl as mtl


def main():
    args = misc_util.parse_args(True)
    bpp = 3

    yuv_file = open(args.tx_url, "rb")
    if not yuv_file:
        print(f"Open {args.tx_url} fail")
        sys.exit(1)

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
    mtl.mtl_para_tx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 1)
    mtl.mtl_para_rx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 0)

    # Create MTL instance
    mtl_handle = mtl.mtl_init(init_para)
    if not mtl_handle:
        print("mtl_init fail")
        sys.exit(1)

    # Create st22p tx session
    tx_para = mtl.st22p_tx_ops()
    tx_para.name = "st22p_tx_python"
    tx_para.width = args.width
    tx_para.height = args.height
    tx_para.fps = mtl.ST_FPS_P59_94
    tx_para.interlaced = args.interlaced
    tx_para.framebuff_cnt = 3
    tx_para.input_fmt = args.pipeline_fmt
    tx_para.pack_type = mtl.ST22_PACK_CODESTREAM
    tx_para.codec = mtl.ST22_CODEC_JPEGXS
    tx_para.device = mtl.ST_PLUGIN_DEVICE_AUTO
    tx_para.quality = mtl.ST22_QUALITY_MODE_QUALITY
    tx_para.codec_thread_cnt = 2
    tx_para.codestream_size = tx_para.width * tx_para.height * bpp / 8
    if tx_para.interlaced:
        tx_para.codestream_size /= 2
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
    tx_para.flags = mtl.ST22P_TX_FLAG_BLOCK_GET
    # create st22p_tx session
    st22p_tx = mtl.st22p_tx_create(mtl_handle, tx_para)
    if not st22p_tx:
        print("st22p_tx_create fail")
        sys.exit(1)
    frame_sz = mtl.st22p_tx_frame_size(st22p_tx)
    print(f"frame_sz: {hex(frame_sz)}")

    # loop until ctrl-c
    try:
        while True:
            frame = mtl.st22p_tx_get_frame(st22p_tx)
            if frame:
                # print(f"frame addr: {hex(mtl.st_frame_addr_cpuva(frame, 0))}")
                # print(f"frame iova: {hex(mtl.st_frame_iova(frame, 0))}")
                yuv_frame = yuv_file.read(frame_sz)
                if not yuv_frame:
                    yuv_file.seek(0)
                    # print("EOF")
                    yuv_frame = yuv_file.read(frame_sz)
                if yuv_frame:
                    # copy yuv data to mtl frame
                    misc_util.copy_to_st_frame(yuv_frame, frame)
                    if args.display:
                        misc_util.frame_display(
                            mtl_handle, frame, args.display_scale_factor
                        )
                else:
                    print(f"Fail to read {hex(frame_sz)} from {args.tx_url}")
                mtl.st22p_tx_put_frame(st22p_tx, frame)
            else:
                print("st22p_tx_get_frame get fail")

    except KeyboardInterrupt:
        print("KeyboardInterrupt")

    # Free st22p_tx session
    mtl.st22p_tx_free(st22p_tx)

    # Close file
    yuv_file.close()

    # Free MTL instance
    mtl.mtl_uninit(mtl_handle)

    print("Everything fine, bye")


if __name__ == "__main__":
    main()
