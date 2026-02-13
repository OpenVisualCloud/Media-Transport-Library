# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

sources = files('tests.cpp', 'st_test.cpp', 'st22_test.cpp', 'st30_test.cpp',
                'st40_test.cpp', 'dma_test.cpp', 'cvt_test.cpp', 'st22p_test.cpp',
                'st20p_test.cpp', 'test_util.cpp', 'sch_test.cpp', 'st30p_test.cpp')

if get_option('buildtype') != 'release'
  add_global_arguments('-DMTL_SIMULATE_PACKET_DROPS=1', language : 'c')
  add_global_arguments('-DMTL_SIMULATE_PACKET_DROPS=1', language : 'cpp')
endif

subdir('st20')
sources += st20_sources

subdir('noctx')
sources += noctx_sources
test_inc = include_directories('.', 'noctx', 'st20')

ufd_sources = files('ufd_test.cpp', 'ufd_loop_test.cpp', 'test_util.cpp')

upl_sources = files('upl_test.cpp', 'upl_loop_test.cpp', 'test_util.cpp')