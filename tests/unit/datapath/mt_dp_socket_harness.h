/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2026 Intel Corporation
 */

#ifndef TESTS_UNIT_DATAPATH_MT_DP_SOCKET_HARNESS_H
#define TESTS_UNIT_DATAPATH_MT_DP_SOCKET_HARNESS_H

/*
 * C API for driving the kernel-socket TX path (mt_dp_socket.c) over loopback.
 * Needs no NIC; SO_BINDTODEVICE needs no root on Linux 5.7 and later.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Builds a single-thread, GSO-enabled TX socket entry the way mt_tx_socket_get()
 * does for a flow to 127.0.0.1:<port A>, then bursts pkt_count GSO-sized
 * packets whose own UDP headers address 127.0.0.1:<port B>. Returns the count
 * mt_tx_socket_burst() accepted and sets *flow_port_datagrams and
 * *hdr_port_datagrams to what the receivers on A and B got. Returns -ENODEV if
 * the host refuses the loopback sockets, another negative errno on other setup
 * failures. */
int ut_dps_gso_burst(int pkt_count, int* flow_port_datagrams, int* hdr_port_datagrams);

#ifdef __cplusplus
}
#endif

#endif
