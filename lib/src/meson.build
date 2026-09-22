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

if not is_windows
  # The client of MtlManager. The library compiles the same source the manager
  # installs as libmtlm_client, so there is one record layout and one socket
  # path search, and no new runtime dependency for a user of libmtl.
  sources += files(
    '../../manager/mtlm_client.c',
    '../../manager/mtlm_sock_path.c',
  )
endif

if is_windows
if get_option('enable_tap') == true
  sources += files('mt_tap.c')
endif
  sources += files('../windows/win_posix.c')
endif

subdir('datapath')
subdir('dev')
subdir('st2110')

