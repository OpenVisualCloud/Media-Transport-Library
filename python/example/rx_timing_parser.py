# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

import datetime
import threading
import time

import matplotlib.pyplot as plt
import misc_util
import pymtl as mtl
from matplotlib.widgets import Button


class Dashboard:
    active = False

    def __init__(self, max_histories):
        self.max_histories = max_histories
        # init data
        self.init_data()
        # init vrx plot
        self.fig, self.ax = plt.subplots()
        plt.title("ST2110-20 Rx Timing Parser")
        plt.subplots_adjust(bottom=0.2)
        (self.vrx_max_line,) = self.ax.plot(
            self.times, self.vrx_max_values, label="VRX Max"
        )
        (self.vrx_min_line,) = self.ax.plot(
            self.times, self.vrx_min_values, label="VRX Min"
        )
        (self.vrx_avg_line,) = self.ax.plot(
            self.times, self.vrx_avg_values, label="VRX Avg"
        )
        # init compliance text
        self.ax.legend(loc="upper left")
        self.text_narrow = self.ax.text(
            0.05, -0.2, f"Narrow: {self.narrow_count}", transform=self.ax.transAxes
        )
        self.text_wide = self.ax.text(
            0.35, -0.2, f"Wide: {self.wide_count}", transform=self.ax.transAxes
        )
        self.text_fail = self.ax.text(
            0.65, -0.2, f"Failed: {self.fail_count}", transform=self.ax.transAxes
        )
        # init cleat button
        self.button_ax = self.fig.add_axes([0.81, 0.05, 0.1, 0.075])
        self.button = Button(self.button_ax, "Clear")
        self.button.on_clicked(self.clear_data_event)

        plt.draw()

        self.active = True

    def init_data(self):
        self.data_count = 0
        # init vrx data
        self.times = [datetime.datetime.now()]
        self.vrx_max_values = [0]
        self.vrx_min_values = [0]
        self.vrx_avg_values = [0]
        # init compliance count data
        self.narrow_count = 0
        self.wide_count = 0
        self.fail_count = 0

    def update(self, frame):
        self.update_ui()

    def update_ui(self):
        self.text_narrow.set_text(f"Narrow: {self.narrow_count}")
        self.text_wide.set_text(f"Wide: {self.wide_count}")
        self.text_fail.set_text(f"Failed: {self.fail_count}")

        self.vrx_max_line.set_data(self.times, self.vrx_max_values)
        self.vrx_min_line.set_data(self.times, self.vrx_min_values)
        self.vrx_avg_line.set_data(self.times, self.vrx_avg_values)

        self.ax.relim()
        self.ax.autoscale_view()
        plt.draw()

    def clear_data_event(self, event):
        print("Clear all data")
        self.init_data()
        self.update_ui()

    def update_vrx(self, max, min, avg):
        if self.data_count == 0:
            self.vrx_max_values.pop(0)
            self.vrx_min_values.pop(0)
            self.vrx_avg_values.pop(0)
            self.times.pop(0)
        # Update data
        self.vrx_max_values.append(max)
        self.vrx_min_values.append(min)
        self.vrx_avg_values.append(avg)
        self.times.append(datetime.datetime.now())
        self.data_count += 1
        # print(f"data_count: {self.data_count}")
        if self.data_count > self.max_histories:
            # Remove old data
            self.vrx_max_values.pop(0)
            self.vrx_min_values.pop(0)
            self.vrx_avg_values.pop(0)
            self.times.pop(0)

    def update_compliance(self, narrow, wide, fail):
        self.narrow_count += narrow
        self.wide_count += wide
        self.fail_count += fail


def rx_frame_loop(st20p_rx, update_interval, plot):
    f_idx = 0
    vrx_min = 10000
    vrx_max = -10000
    vrx_avg_sum = 0
    narrow = 0
    wide = 0
    fail = 0

    # loop until ctrl-c

    while plot.active:
        frame = mtl.st20p_rx_get_frame(st20p_rx)
        if frame:
            f_idx += 1
            tp = mtl.st_frame_tp_meta(frame, mtl.MTL_SESSION_PORT_P)
            if tp.vrx_max > vrx_max:
                vrx_max = tp.vrx_max
            if tp.vrx_min < vrx_min:
                vrx_min = tp.vrx_min
            vrx_avg_sum += tp.vrx_avg
            if tp.compliant == mtl.ST_RX_TP_COMPLIANT_NARROW:
                narrow += 1
            elif tp.compliant == mtl.ST_RX_TP_COMPLIANT_WIDE:
                wide += 1
            else:
                fail += 1

            if f_idx > update_interval:
                vrx_avg = vrx_avg_sum // f_idx
                # print(f"vrx_max: {vrx_max}, vrx_min: {vrx_max}, vrx_avg: {vrx_avg}")
                plot.update_vrx(vrx_max, vrx_min, vrx_avg)
                # print(f"narrow: {narrow}, wide: {wide}, fail: {fail}")
                plot.update_compliance(narrow, wide, fail)
                plot.update_ui()

                # reset stat
                f_idx = 0
                vrx_min = 10000
                vrx_max = -10000
                vrx_avg_sum = 0
                narrow = 0
                wide = 0
                fail = 0

            mtl.st20p_rx_put_frame(st20p_rx, frame)
        else:
            # sleep to allow ui draw
            time.sleep(0.005)


def plt_show():
    plt.show()


def main():
    args = misc_util.parse_args(False)

    # initial plot, max 1 hour, 1 item 1 second
    plot = Dashboard(max_histories=60 * 60 * 1)

    # Init mtl para
    init_para = mtl.mtl_init_params()
    mtl.mtl_para_port_set(init_para, mtl.MTL_PORT_P, args.p_port)
    mtl.mtl_para_pmd_set(
        init_para, mtl.MTL_PORT_P, mtl.mtl_pmd_by_port_name(args.p_port)
    )
    init_para.num_ports = 1
    mtl.mtl_para_sip_set(init_para, mtl.MTL_PORT_P, args.p_sip)
    init_para.flags = mtl.MTL_FLAG_BIND_NUMA | mtl.MTL_FLAG_DEV_AUTO_START_STOP
    init_para.flags |= mtl.MTL_FLAG_ENABLE_HW_TIMESTAMP
    if args.ptp:
        init_para.flags |= mtl.MTL_FLAG_PTP_ENABLE
    mtl.mtl_para_tx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 0)
    mtl.mtl_para_rx_queues_cnt_set(init_para, mtl.MTL_PORT_P, 1)
    init_para.nb_rx_desc = args.nb_rx_desc

    # Create MTL instance
    mtl_handle = mtl.mtl_init(init_para)

    # Create st20p rx session
    rx_para = mtl.st20p_rx_ops()
    rx_para.name = "st20p_rx_python"
    rx_para.width = args.width
    rx_para.height = args.height
    rx_para.fps = args.fps
    rx_para.interlaced = args.interlaced
    rx_para.framebuff_cnt = 3
    rx_para.transport_fmt = mtl.ST20_FMT_YUV_422_10BIT
    rx_para.output_fmt = args.pipeline_fmt
    rx_para.rx_burst_size = args.rx_burst_size
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
    # rx_para.flags = mtl.ST20P_RX_FLAG_BLOCK_GET
    # enable timing parser meta
    rx_para.flags |= mtl.ST20P_RX_FLAG_TIMING_PARSER_META
    # create st20p_rx session
    st20p_rx = mtl.st20p_rx_create(mtl_handle, rx_para)

    # update plot per 1 s
    framerate = mtl.st_frame_rate(rx_para.fps)
    rx_frame_thread = threading.Thread(
        target=rx_frame_loop, args=(st20p_rx, framerate, plot)
    )
    rx_frame_thread.start()

    # show the plt
    print("plt_show start")
    plt_show()
    plot.active = False
    print("plt_show end")

    print("Wait rx_frame_thread")
    rx_frame_thread.join()

    # Free st20p_rx session
    mtl.st20p_rx_free(st20p_rx)

    # Free MTL instance
    mtl.mtl_uninit(mtl_handle)

    print("Everything fine, bye")


if __name__ == "__main__":
    main()
