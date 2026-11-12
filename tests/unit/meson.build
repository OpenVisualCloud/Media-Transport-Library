# SPDX-License-Identifier: BSD-3-Clause

add_languages('cpp', required : true)

unit_cpp_args = ['-Wall', '-Werror']
unit_c_args = ['-Wall', '-Werror']
# Deliberately no '-Wl,--allow-multiple-definition': harnesses shadow production symbols by
# #define rename, so a real duplicate must fail the link.
unit_link_args = []

# Compiler hardening, see doc/build.md. lib/meson.build sets it for the C sources; the C++
# sources and the C++ link of UnitTest take it as target arguments.
if not is_windows
  unit_cpp_args += ['-fstack-protector-strong', '-fstack-clash-protection', '-fcf-protection=full',
    '-Wformat', '-Wformat-security', '-Werror=format-security', '-Wimplicit-fallthrough',
    '-D_GLIBCXX_ASSERTIONS']
  if get_option('optimization') not in ['0', 'plain'] and get_option('b_sanitize') == 'none' and not ' '.join(get_option('cpp_args')).contains('_FORTIFY_SOURCE')
    fortify = meson.get_compiler('cpp').get_define('__USE_FORTIFY_LEVEL', prefix : '#include <string.h>',
      args : ['-O2', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3']) == '3' ? '3' : '2'
    unit_cpp_args += ['-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=' + fortify]
  endif
  unit_link_args += ['-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack']
endif

unit_build_rpath = ''
if not is_windows
  # rte_eal_init() dlopens every driver in RTE_EAL_PMD_PATH and those drivers request the bus
  # libraries by soname, so a second DPDK on the host ld.so path loads one bus library twice and
  # its RTE_REGISTER_TAILQ constructor panics. Pin the process to the DPDK this binary compiled
  # against: --disable-new-dtags turns Meson's rpath into DT_RPATH, the only rpath ld.so searches
  # when resolving a dlopened driver's own dependencies.
  # Both are for an ELF host alone: Windows has no rpath, and the ld of mingw-w64 stops with
  # "unrecognized option '--disable-new-dtags'".
  unit_link_args += '-Wl,--disable-new-dtags'
  unit_build_rpath = dpdk_dep.get_variable(pkgconfig : 'libdir')
endif

unit_include_dirs = [
  include_directories('../../include'),
  include_directories('../../lib/src'),
  include_directories('.'),
]

unit_mtl_dep = mtl_internal_dep
unit_dpdk_dep = dpdk_dep
unit_platform_deps = []

if is_windows
  # The C harnesses and the C++ tests cannot share one include path on Windows.
  # A harness compiles a production source in place, so it needs the include
  # path of the library: the project root, lib/windows, and the Windows EAL
  # directory of DPDK. The C++ tests must have none of the three:
  #
  #  * The project root holds a file that is named VERSION. A Windows file
  #    system is not case sensitive, so the #include <version> of libstdc++
  #    reads that file, and the compiler stops with "too many decimal points
  #    in number".
  #  * pthread.h of the Windows EAL directory of DPDK is a shim for MSVC. It
  #    hides the real header of mingw-w64, which libstdc++ needs for <mutex>
  #    and <thread>, so every C++ unit fails in gthr-default.h.
  #
  # Meson has one include path per target and not one per language, so the
  # three directories go to the C compiler alone, as -I in c_args, and the
  # target takes the library without its include path.
  unit_c_args += ['-DWINDOWSENV',
                  '-I' + meson.project_source_root(),
                  '-I' + meson.project_build_root(),
                  '-I' + meson.project_source_root() + '''\lib\windows''',
                  '-I' + meson.project_source_root() + '''\lib\''' +
                  get_option('dpdk_root_dir') + '''\lib\eal\windows\include''']
  unit_mtl_dep = declare_dependency(link_with : mtl_lib)
  # libmtl.dll holds a static DPDK, and the linker of mingw-w64 exports every
  # symbol of a DLL that has no explicit export list, so the import library
  # libmtl.dll.a holds rte_eal_init and the whole of DPDK as well. A link
  # against the DPDK static libraries too gives "multiple definition" for each
  # of about 100 names, and for each harness stub of a rte_ function. So the
  # binary takes the headers of DPDK and not its libraries: every rte_ symbol
  # comes from libmtl.dll, which also keeps one EAL in the process.
  unit_dpdk_dep = dpdk_dep.partial_dependency(compile_args : true, includes : true)
  # htons, ntohl and inet_pton of the harnesses. libmtl.dll links ws2_32 for
  # itself, and each of these comes by name from ws2_32 and not from the DLL.
  unit_platform_deps += ws2_32_dep
  # dlopen() of the plugin harness. libmtl.dll links dlfcn for itself.
  unit_platform_deps += libdl_dep
  # mtl_api.h asks for mtl_build_config.h, which the root of the build tree
  # holds. A copy of that header here answers it, because this directory is an
  # include directory of UnitTest, so the C++ path needs no project root.
  configure_file(output : 'mtl_build_config.h', configuration : mtl_conf)
endif

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
  'common/ut_tx_ring.c',
  'common/ut_tx_ring_test.cpp',
  'common/mt_bitmap_harness.c',
  'common/mt_bitmap_test.cpp',
  'common/mt_mempool_harness.c',
  'common/mt_mempool_test.cpp',
  'dev/mt_dev_harness.c',
  'dev/mt_dev_igc_test.cpp',
  'dev/mt_dev_rx_desc_test.cpp',
  'dev/mt_dev_tx_desc_test.cpp',
  'dev/mt_dev_tx_pool_test.cpp',
  'dev/mt_dev_tx_reclaim_test.cpp',
  'dev/mt_dev_tx_flush_test.cpp',
  'dev/mt_dev_tx_done_cleanup_test.cpp',
  'dev/mt_dev_rss_reta_test.cpp',
  'dev/mt_dev_devargs_test.cpp',
  'dev/mt_dev_eal_argv_test.cpp',
  'dev/mt_dev_eal_pmd_dir_test.cpp',
  'dev/iavf_intr_unregister_test.cpp',
  'dev/mt_dev_port_stats_test.cpp',
  'dev/mt_dev_pacing_test.cpp',
  'dev/mt_dev_set_tx_bps_test.cpp',
  'dev/mt_flow_harness.c',
  'dev/mt_flow_retry_test.cpp',
  'datapath/mt_dp_socket_harness.c',
  'datapath/mt_dp_socket_tx_test.cpp',
  'datapath/mt_dp_socket_rx_test.cpp',
  'datapath/mt_queue_harness.c',
  'datapath/mt_queue_test.cpp',
  'fmt/st_fmt_test.cpp',
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
  'session/st40/fmd_test.cpp',
  'session/st40_tx_harness.c',
  'session/st40_tx/pacing_test.cpp',
  'session/st40_tx/pool_stall_test.cpp',
  'session/st40_tx/rtp_gate_snap_test.cpp',
  'session/st40_tx/fmd_build_packet_test.cpp',
  'session/st40_tx/redundant_alloc_test.cpp',
  'session/st30_harness.c',
  'session/st30/redundancy_test.cpp',
  'session/st30/timestamp_test.cpp',
  'session/st30/header_validation_test.cpp',
  'session/st30/stats_test.cpp',
  'session/st30/reorder_test.cpp',
  'session/st30/err_packets_test.cpp',
  'session/st30/corruption_test.cpp',
  'session/st30/bitmap_bounds_test.cpp',
  'session/st30_tx_harness.c',
  'session/st30_tx/pacing_test.cpp',
  'session/st30_tx/pool_stall_test.cpp',
  'session/st30_tx/rl_profiling_test.cpp',
  'session/st30_tx/redundant_alloc_test.cpp',
  'session/st30_tx/rl_pads_test.cpp',
  'session/st20_harness.c',
  'session/st20/slot_test.cpp',
  'session/st20/redundancy_test.cpp',
  'session/st20/bitmap_test.cpp',
  'session/st20/bitmap_bounds_test.cpp',
  'session/st20/header_validation_test.cpp',
  'session/st20/reorder_test.cpp',
  'session/st20/stats_test.cpp',
  'session/st20/loss_estimate_test.cpp',
  'session/st20/err_packets_test.cpp',
  'session/st20/timestamp_source_test.cpp',
  'session/st20/tp_burst_test.cpp',
  'session/st20/tp_rtp_ts_delta_test.cpp',
  'session/st20/tp_stat_test.cpp',
  'session/st20/interlaced_tp_phase_test.cpp',
  'session/st20/st22_box_parse_test.cpp',
  'session/st20/st22_pkt_counter_test.cpp',
  'session/st20/st20_first_pkt_idx_test.cpp',
  'session/st20/st20_user_meta_test.cpp',
  'session/st20/dma_test.cpp',
  'session/st20/ext_frame_test.cpp',
  'session/st20/callback_refusal_test.cpp',
  'session/st20/dma_offload_test.cpp',
  'session/st20/frame_memcpy_test.cpp',
  'session/st20/stat_dump_test.cpp',
  'session/st20_tx_harness.c',
  'session/st20_tx/epoch_test.cpp',
  'session/st20_tx/interlaced_field_epoch_test.cpp',
  'session/st20_tx/pacing_test.cpp',
  'session/st20_tx/frame_stamp_test.cpp',
  'session/st20_tx/frame_iova_test.cpp',
  'session/st20_tx/zero_copy_test.cpp',
  'session/st20_tx/zero_copy_lazy_test.cpp',
  'session/st20_tx/pool_stall_test.cpp',
  'session/st20_tx/zero_copy_reclaim_test.cpp',
  'session/st20_tx/queue_recovery_test.cpp',
  'session/st20_tx/rl_training_test.cpp',
  'session/st20_tx/rtcp_pool_test.cpp',
  'session/st_video_transmitter_harness.c',
  'session/st20_tx/rl_warm_up_test.cpp',
  'session/st20_tx/rl_model_test.cpp',
  'session/st20_tx/tx_queue_stall_test.cpp',
  'session/st20_tx/transmitter_bugs_test.cpp',
  'session/st20_tx/rtp_timestamp_rounding_test.cpp',
  'session/st20_tx/rtp_gate_snap_test.cpp',
  'session/st20_tx/mempool_release_test.cpp',
  'session/st20_tx/stat_dump_test.cpp',
  'session/st20_tx/ext_frame_done_test.cpp',
  'session/st20_tx/tasklet_pending_test.cpp',
  'session/st20_tx/init_hw_test.cpp',
  'session/st20_tx/update_destination_test.cpp',
  'session/st20_tx/session_lock_test.cpp',
  'session/multi_essence_sync_test.cpp',
  'session/rx_redundant_ip_test.cpp',
  'session/rtcp_tx_harness.c',
  'session/rtcp/parse_nack_bounds_test.cpp',
  'session/rtcp_attack_harness.c',
  'session/rtcp/nack_attack_test.cpp',
  'session/rtcp/copy_pool_test.cpp',
  'pipeline/st20p_harness.c',
  'pipeline/st20p_test.cpp',
  'pipeline/st20p_rx_concurrency_test.cpp',
  'pipeline/st20p_rx_blocking_test.cpp',
  'pipeline/st20p_rx_pkt_convert_test.cpp',
  'pipeline/st20p_tx_harness.c',
  'pipeline/st20p_tx_concurrency_test.cpp',
  'pipeline/st20p_tx_blocking_test.cpp',
  'pipeline/st20p_tx_ext_frame_release_test.cpp',
  'pipeline/st20p_tx_callback_free_test.cpp',
  'pipeline/st20p_concurrency_test.cpp',
  'pipeline/st20p_concurrency_stress_test.cpp',
  'pipeline/st_convert_harness.c',
  'pipeline/st20p_nt_convert_test.cpp',
  'pipeline/st30p_harness.c',
  'pipeline/st30p_test.cpp',
  'pipeline/st30p_tx_harness.c',
  'pipeline/st30p_tx_blocking_test.cpp',
  'pipeline/st30p_rx_blocking_test.cpp',
  'pipeline/st30p_tx_create_test.cpp',
  'pipeline/st30p_concurrency_test.cpp',
  'pipeline/st40p_harness.c',
  'pipeline/st40p_test.cpp',
  'pipeline/st40p_tx_harness.c',
  'pipeline/st40p_tx_blocking_test.cpp',
  'pipeline/st40p_rx_blocking_test.cpp',
  'pipeline/st40p_tx_fbs_test.cpp',
  'pipeline/st40p_concurrency_test.cpp',
  'pipeline/st22p_harness.c',
  'pipeline/st22p_tx_harness.c',
  'pipeline/st22p_tx_drop_when_late_test.cpp',
  'pipeline/st22p_rx_blocking_test.cpp',
  'pipeline/st22p_tx_blocking_test.cpp',
  'pipeline/st22p_tx_stale_wake_test.cpp',
  'pipeline/st22p_create_test.cpp',
  'pipeline/st22p_concurrency_test.cpp',
  'pipeline/st22p_concurrency_stress_test.cpp',
  'pipeline/tx_notify_frame_late_test.cpp',
  'pipeline/tx_wake_block_test.cpp',
  'pipeline/stale_handle_stats_test.cpp',
  'pipeline/tx_frame_done_notify_test.cpp',
  'pipeline/st_plugin_harness.c',
  'pipeline/st_plugin_session_test.cpp',
  'sch/mt_sch_harness.c',
  'sch/mtl_sch_api_test.cpp',
  'sch/mt_sch_mempolicy_test.cpp',
  'sch/mt_sch_filelock_test.cpp',
  'sch/mt_sch_lcore_test.cpp',
  'sch/mt_sch_quiescent_test.cpp',
  'ptp/ptp_harness.c',
  'ptp/servo_test.cpp',
  'ptp/adjust_delta_test.cpp',
  'ptp/user_sync_test.cpp',
  'ptp/delta_math_test.cpp',
  'ptp/t3_test.cpp',
  'main/tsc_calibrate_harness.c',
  'main/tsc_calibrate_test.cpp',
  'main/mtl_init_params_test.cpp',
  'main/mt_instance_harness.c',
  'main/mt_instance_test.cpp',
  'dma/mt_map_harness.c',
  'dma/mt_map_test.cpp',
  'dma/mt_dma_auto_harness.c',
  'dma/mt_dma_auto_test.cpp',
  'main.cpp',
]

if is_windows
  # MTL builds these paths on Linux alone, or mingw-w64 lacks the call that the
  # unit needs: the kernel socket backend, the dlopen() PMD directory and the ELF
  # symbols of the iavf PMD, the sysfs DMA probe, the UNIX socket instance, the
  # NUMA memory policy and lcore affinity of the scheduler, mprotect() and
  # open_memstream().
  unit_posix_only = [
    'dev/mt_dev_eal_pmd_dir_test.cpp',
    'dev/iavf_intr_unregister_test.cpp',
    'datapath/mt_dp_socket_harness.c',
    'datapath/mt_dp_socket_tx_test.cpp',
    'datapath/mt_dp_socket_rx_test.cpp',
    'dma/mt_dma_auto_harness.c',
    'dma/mt_dma_auto_test.cpp',
    'main/mt_instance_harness.c',
    'main/mt_instance_test.cpp',
    'sch/mt_sch_harness.c',
    'sch/mtl_sch_api_test.cpp',
    'sch/mt_sch_mempolicy_test.cpp',
    'sch/mt_sch_filelock_test.cpp',
    'sch/mt_sch_lcore_test.cpp',
    'session/rtcp_attack_harness.c',
    'session/rtcp/nack_attack_test.cpp',
    'session/rtcp/copy_pool_test.cpp',
    'pipeline/st40p_tx_fbs_test.cpp',
  ]
  unit_windows_sources = []
  foreach src : unit_sources
    if src not in unit_posix_only
      unit_windows_sources += src
    endif
  endforeach
  # With no MtlManager on Windows, processes share lcores through the shims of lib/windows.
  unit_sources = unit_windows_sources + [
    'sch/mt_sch_win_harness.c',
    'sch/mt_sch_win_lcore_test.cpp',
  ]
endif

executable('UnitTest', unit_sources,
  c_args : unit_c_args,
  cpp_args : unit_cpp_args,
  link_args : unit_link_args,
  build_rpath : unit_build_rpath,
  include_directories : unit_include_dirs,
  dependencies : [unit_mtl_dep, unit_dpdk_dep, libm_dep, libnuma_dep, gtest_dep, gmock_dep,
                  libpthread_dep, usdt_provider_header_dep] + unit_platform_deps + unit_extra_deps,
  install : false)
