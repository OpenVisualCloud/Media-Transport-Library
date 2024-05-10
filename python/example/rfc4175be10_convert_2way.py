# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# Usage: python3 rfc4175be10_convert_2way.py --tx_url yuv422rfc4175be10_1080p.yuv --display_scale_factor 2

"""Module of MTL 2 way convert python example."""

import sys

import misc_util
import pymtl as mtl


def main():
    args = misc_util.parse_args(False)

    yuv_file = open(args.tx_url, "rb")
    if not yuv_file:
        print(f"Open {args.tx_url} fail")
        sys.exit(1)

    # mtl.ST_FRAME_FMT_YUV422RFC4175PG2BE10 or others
    rfc4175_fmt = mtl.st_frame_fmt_from_transport(args.transport_fmt)
    rfc4175_frame = mtl.st_frame_create_by_malloc(
        rfc4175_fmt,
        args.width,
        args.height,
        args.interlaced,
    )
    if not rfc4175_frame:
        print(f"Create rfc4175_frame fail, fmt: {mtl.st_frame_fmt_name(rfc4175_fmt)}")
        sys.exit(1)

    # read yuv to rfc4175_frame
    rfc4175_frame_ptr = yuv_file.read(rfc4175_frame.data_size)
    if not rfc4175_frame_ptr:
        print(f"Read {rfc4175_frame.data_size} from {args.tx_url} fail")
        sys.exit(1)
    misc_util.copy_to_st_frame(rfc4175_frame_ptr, rfc4175_frame)

    # ST_FRAME_FMT_YUV422PLANAR10LE or mtl.ST_FRAME_FMT_V210
    target_fmt = args.pipeline_fmt
    target_frame = mtl.st_frame_create_by_malloc(
        target_fmt,
        args.width,
        args.height,
        args.interlaced,
    )
    if not target_frame:
        print(f"Create target_frame fail, fmt: {mtl.st_frame_fmt_name(target_fmt)}")
        sys.exit(1)
    target_frame_decimated = mtl.st_frame_create_by_malloc(
        target_fmt,
        args.width / args.display_scale_factor,
        args.height / args.display_scale_factor,
        args.interlaced,
    )
    if not target_frame_decimated:
        print(f"Create decimated frame fail, fmt: {mtl.st_frame_fmt_name(target_fmt)}")
        sys.exit(1)

    print(
        f"Read rfc4175 success from {args.tx_url}, w: {args.width}, h: {args.height}, "
        f"sz: {hex(rfc4175_frame.data_size)}, fmt: {mtl.st_frame_fmt_name(rfc4175_frame.fmt)}"
    )

    # the 2 way convert
    if target_frame.fmt == mtl.ST_FRAME_FMT_YUV422PLANAR10LE:
        mtl.st20_rfc4175_422be10_to_yuv422p10le_2way_cpuva(
            mtl.st_frame_addr_cpuva(rfc4175_frame, 0),
            mtl.st_frame_addr_cpuva(target_frame, 0),
            mtl.st_frame_addr_cpuva(target_frame, 1),
            mtl.st_frame_addr_cpuva(target_frame, 2),
            args.width,
            args.height,
            mtl.st_frame_addr_cpuva(target_frame_decimated, 0),
            mtl.st_frame_addr_cpuva(target_frame_decimated, 1),
            mtl.st_frame_addr_cpuva(target_frame_decimated, 2),
            args.display_scale_factor,
        )
    elif target_frame.fmt == mtl.ST_FRAME_FMT_V210:
        mtl.st20_rfc4175_422be10_to_v210_2way_cpuva(
            mtl.st_frame_addr_cpuva(rfc4175_frame, 0),
            mtl.st_frame_addr_cpuva(target_frame, 0),
            args.width,
            args.height,
            mtl.st_frame_addr_cpuva(target_frame_decimated, 0),
            args.display_scale_factor,
        )
    else:
        print(f"Unknown fmt: {mtl.st_frame_fmt_name(target_frame.fmt)}")

    misc_util.frame_display(target_frame, args.display_scale_factor)
    print(
        f"Full frame(fmt: {mtl.st_frame_fmt_name(target_frame.fmt)}) displayed now, any key to next decimated"
    )
    misc_util.wait_key()
    misc_util.frame_display(target_frame_decimated, 1)
    misc_util.wait_key()
    print(
        f"Decimated frame(fmt: {mtl.st_frame_fmt_name(target_frame.fmt)}) displayed now, any key to exit"
    )
    misc_util.destroy()

    print("Everything fine, bye")


if __name__ == "__main__":
    main()
