# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

sources = files(
	'st_main.c',
	'st_dev.c',
	'st_sch.c',
	'st_cni.c',
	'st_ptp.c',
	'st_fmt.c',
	'st_arp.c',
	'st_mcast.c',
	'st_util.c',
	'st_tx_video_session.c',
	'st_video_transmitter.c',
	'st_rx_video_session.c',
	'st_tx_audio_session.c',
	'st_audio_transmitter.c',
	'st_rx_audio_session.c',
	'st_tx_ancillary_session.c',
	'st_rx_ancillary_session.c',
	'st_ancillary_transmitter.c',
	'st_ancillary.c',
	'st_dma.c',
	'st_avx2.c',
	'st_avx512.c',
	'st_avx512_vbmi.c',
	'st_admin.c',
	'st_config.c',
	'st_socket.c',
)

if get_option('enable_kni') == true
  sources += files('st_kni.c')
endif

if is_windows
if get_option('enable_tap') == true
  sources += files('st_tap.c')
endif
  sources += files('../windows/win_posix.c')
endif

subdir('pipeline')
subdir('redundant')