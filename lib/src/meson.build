# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

sources = files(
  'mt_main.c',
  'mt_sch.c',
  'mt_cni.c',
  'mt_ptp.c',
  'mt_arp.c',
  'mt_dhcp.c',
  'mt_mcast.c',
  'mt_util.c',
  'mt_dma.c',
  'mt_admin.c',
  'mt_config.c',
  'mt_socket.c',
  'mt_stat.c',
  'mt_rtcp.c',
  'mt_flow.c',
  'mt_instance.c',
  'mt_log.c',
  'mt_pcap.c',
)

if is_windows
if get_option('enable_tap') == true
  sources += files('mt_tap.c')
endif
  sources += files('../windows/win_posix.c')
endif

subdir('datapath')
subdir('dev')
subdir('st2110')
subdir('deprecated/udp')

