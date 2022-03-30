# Copyright (C) 2021 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials,
# and your use of them is governed by the express license under which they
# were provided to you ("License").
# Unless the License provides otherwise, you may not use, modify, copy,
# publish, distribute, disclose or transmit this software or the related
# documents without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no
# express or implied warranties, other than those that are expressly stated
# in the License.

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
	'st_avx512.c',
	'st_avx512_vbmi.c',
)

if get_option('enable_kni') == true
  sources += files('st_kni.c')
endif

if is_windows
  sources += files('../windows/win_posix.c')
endif