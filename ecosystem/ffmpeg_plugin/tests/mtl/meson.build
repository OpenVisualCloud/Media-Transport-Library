# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# <mtl/*.h> from this checkout: the include closure of mtl_common.h, and a
# mtl_build_config.h with no value, as the tests expand no MTL_VERSION.
foreach header : ['mtl_api.h', 'st_api.h', 'st20_api.h', 'st30_api.h', 'st_pipeline_api.h']
  configure_file(input : '../../../../include/' + header, output : header, copy : true)
endforeach
configure_file(output : 'mtl_build_config.h', configuration : configuration_data())
