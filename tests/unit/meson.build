# SPDX-License-Identifier: BSD-3-Clause

add_languages('cpp', required : true)

unit_cpp_args = ['-Wall', '-Werror']
unit_c_args = ['-Wall', '-Werror']
# Deliberately no '-Wl,--allow-multiple-definition': harnesses shadow production symbols by
# #define rename, so a real duplicate must fail the link.
unit_link_args = []

# rte_eal_init() dlopens every driver in RTE_EAL_PMD_PATH and those drivers request the bus
# libraries by soname, so a second DPDK on the host ld.so path loads one bus library twice and
# its RTE_REGISTER_TAILQ constructor panics. Pin the process to the DPDK this binary compiled
# against: --disable-new-dtags turns Meson's rpath into DT_RPATH, the only rpath ld.so searches
# when resolving a dlopened driver's own dependencies.
unit_link_args += '-Wl,--disable-new-dtags'
unit_build_rpath = dpdk_dep.get_variable(pkgconfig : 'libdir')

unit_include_dirs = [
  include_directories('../../include'),
  include_directories('../../lib/src'),
  include_directories('.'),
]

# Answers the <mtl/*.h> of the ecosystem sources the harnesses compile in place.
# It writes into the build side of include_directories('.') above, so it must run
# before the executable that reads it.
subdir('mtl')

gtest_dep = dependency('gtest', required : true)
gmock_dep = dependency('gmock', required : true)
libpthread_dep = meson.get_compiler('c').find_library('pthread', required : true)

# When GPU-direct is enabled, the included production .c files reference
# gpu_allocate_shared_buffer; link the gpu_direct DSO so --no-undefined passes.
unit_extra_deps = []
if gpu_direct_dep.found()
  unit_extra_deps += gpu_direct_dep
endif

unit_sources = [
  'common/ut_common.c',
  'ffmpeg/mtl_common_harness.c',
  'ffmpeg/mtl_common_test.cpp',
  'dev/mt_dev_harness.c',
  'dev/mt_dev_igc_test.cpp',
  'dev/mt_dev_devargs_test.cpp',
  'session/st40_harness.c',
  'session/st40_tx_test_harness.c',
  'session/st40/redundancy_test.cpp',
  'session/st40/timestamp_test.cpp',
  'session/st40/header_validation_test.cpp',
  'session/st40/stats_test.cpp',
  'session/st40/reorder_test.cpp',
  'session/st40/f_bits_test.cpp',
  'session/st40/marker_test.cpp',
  'session/st40/prev_window_test.cpp',
  'session/st40/bitmap_test.cpp',
  'session/st40/err_packets_test.cpp',
  'session/st40/frame_assembly_test.cpp',
  'session/st40/rfc8331_payload_bytes_test.cpp',
  'session/st40/rfc8331_codec_test.cpp',
  'session/st40/tx_corrupt_parity_test.cpp',
  'session/st40_tx_harness.c',
  'session/st40_tx/pacing_test.cpp',
  'session/st40_tx/rtp_gate_snap_test.cpp',
  'session/st30_harness.c',
  'session/st30/redundancy_test.cpp',
  'session/st30/timestamp_test.cpp',
  'session/st30/header_validation_test.cpp',
  'session/st30/stats_test.cpp',
  'session/st30/reorder_test.cpp',
  'session/st30/err_packets_test.cpp',
  'session/st30/corruption_test.cpp',
  'session/st20_harness.c',
  'session/st20/slot_test.cpp',
  'session/st20/redundancy_test.cpp',
  'session/st20/bitmap_test.cpp',
  'session/st20/header_validation_test.cpp',
  'session/st20/reorder_test.cpp',
  'session/st20/stats_test.cpp',
  'session/st20/err_packets_test.cpp',
  'session/st20/timestamp_source_test.cpp',
  'session/st20/st22_box_parse_test.cpp',
  'session/st20_tx_harness.c',
  'session/st20_tx/epoch_test.cpp',
  'session/st20_tx/pacing_test.cpp',
  'session/st_video_transmitter_harness.c',
  'session/st20_tx/rl_warm_up_test.cpp',
  'session/st20_tx/transmitter_bugs_test.cpp',
  'session/st20_tx/rtp_timestamp_rounding_test.cpp',
  'session/st20_tx/rtp_gate_snap_test.cpp',
  'session/st20_tx/mempool_release_test.cpp',
  'pipeline/st20p_harness.c',
  'pipeline/st20p_test.cpp',
  'pipeline/st20p_rx_concurrency_test.cpp',
  'pipeline/st20p_tx_harness.c',
  'pipeline/st20p_tx_concurrency_test.cpp',
  'pipeline/st20p_tx_blocking_test.cpp',
  'pipeline/st20p_tx_ext_frame_release_test.cpp',
  'pipeline/st20p_concurrency_test.cpp',
  'pipeline/st20p_concurrency_stress_test.cpp',
  'pipeline/st30p_harness.c',
  'pipeline/st30p_test.cpp',
  'pipeline/st30p_tx_harness.c',
  'pipeline/st30p_concurrency_test.cpp',
  'pipeline/st40p_harness.c',
  'pipeline/st40p_test.cpp',
  'pipeline/st40p_tx_harness.c',
  'pipeline/st40p_concurrency_test.cpp',
  'pipeline/st22p_harness.c',
  'pipeline/st22p_tx_harness.c',
  'pipeline/st22p_tx_drop_when_late_test.cpp',
  'pipeline/st22p_concurrency_test.cpp',
  'pipeline/st22p_concurrency_stress_test.cpp',
  'ptp/ptp_harness.c',
  'ptp/servo_test.cpp',
  'ptp/adjust_delta_test.cpp',
  'ptp/user_sync_test.cpp',
  'ptp/delta_math_test.cpp',
  'ptp/t3_test.cpp',
  'main.cpp',
]

executable('UnitTest', unit_sources,
  c_args : unit_c_args,
  cpp_args : unit_cpp_args,
  link_args : unit_link_args,
  build_rpath : unit_build_rpath,
  include_directories : unit_include_dirs,
  dependencies : [mtl_internal_dep, dpdk_dep, libm_dep, gtest_dep, gmock_dep, libpthread_dep,
                  usdt_provider_header_dep] + unit_extra_deps,
  install : false)
