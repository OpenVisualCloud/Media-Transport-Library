# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The <mtl/experimental/...> part of the view in ../meson.build. Its headers
# include "../st20_api.h", which then is the copy in the parent directory.
foreach mtl_view_header : mtl_experimental_header_files
  configure_file(input : mtl_view_header, output : '@PLAINNAME@', copy : true)
endforeach
