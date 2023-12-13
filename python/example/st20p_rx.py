# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

import time

import pymtl as mtl

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

# loop until ctrl-c
try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("KeyboardInterrupt")

# Free MTL instance
mtl.mtl_uninit(mtl_handle)

print("Everythin fine, bye")
