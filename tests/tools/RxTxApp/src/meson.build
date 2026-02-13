# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

sources = files('rxtx_app.c', 'args.c', 'parse_json.c', 'fmt.c',
				'tx_st20p_app.c', 'rx_st20p_app.c',
				'tx_st22p_app.c', 'rx_st22p_app.c',
				'tx_ancillary_app.c', 'rx_ancillary_app.c',
				'tx_fastmetadata_app.c', 'rx_fastmetadata_app.c',
				'tx_st30p_app.c', 'rx_st30p_app.c',
				'tx_st40p_app.c', 'rx_st40p_app.c',
				)

if app_has_sdl2
  sources += files('player.c')
endif

subdir('legacy')
subdir('experimental')
