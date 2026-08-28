# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# The ecosystem sources that a unit harness compiles in place -- the FFmpeg
# plugin is the first -- ask for the public API as <mtl/*.h>, the path of an
# installed MTL. The unit tier installs nothing and CI gives it a host with no
# MTL on the compiler's system path, so this directory makes the same view
# inside the build directory: tests/unit is already an include directory of
# UnitTest, so a copy of each public header here answers <mtl/...> with the
# header of this checkout. It takes the same file list install_headers() takes,
# so the view and the installed tree cannot drift.
#
# A quoted include inside a copied header -- "st20_api.h", "mtl_build_config.h"
# -- falls back to the include path of UnitTest, which holds both the source
# include directory and the build root. So only the top-level headers need a
# copy here.
foreach mtl_view_header : mtl_header_files
  configure_file(input : mtl_view_header, output : '@PLAINNAME@', copy : true)
endforeach
