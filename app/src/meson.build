# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

sources = files('rxtx_app.c', 'tx_ancillary_app.c', 'tx_audio_app.c',
	'tx_video_app.c', 'args.c', 'parse_json.c', 'rx_video_app.c',
	'rx_audio_app.c', 'rx_ancillary_app.c', 'tx_st22_app.c', 'rx_st22_app.c',
	'tx_st22p_app.c', 'rx_st22p_app.c', 'tx_st20p_app.c', 'rx_st20p_app.c',
	'rx_st20r_app.c', 'fmt.c', )

if app_has_sdl2
  sources += files('player.c')
endif