# SPDX-License-Identifier: BSD-3-Clause

# Per-function unit tests.
#
# Each module:
#   * pulls one (or two) production .c file(s) into a wrapper TU,
#   * exposes the function-under-test via an extern "C" bridge,
#   * is linked with -Wl,--gc-sections so unused production functions
#     and their externs are dropped at link time — no libmtl link.
#
# To add a module:
#   1. Drop wrap_<file>.c + <name>_test.cpp under tests/unit/<name>/
#   2. Add an entry to `unit_modules` below. Done — no per-subdir
#      meson.build needed.

add_languages('cpp', required : true)

# Vendor FFF (Fake Function Framework, MIT) at configure time. Not
# committed; fetched on first configure and reused. Override with
# -Dfff_url=... for offline mirrors.
fff_url = 'https://raw.githubusercontent.com/meekrosoft/fff/master/fff.h'
fff_h_path = meson.current_source_dir() / 'fff.h'
run_command('sh', '-c',
  'test -s "' + fff_h_path + '" || curl --fail --silent --show-error --location ' +
  '--output "' + fff_h_path + '" "' + fff_url + '"',
  check : true)

unit_include_dirs = [
  mtl_include_dir,                   # project root + ./include + build root
  include_directories('../../lib/src'),
  include_directories('.'),          # for fff.h
]

gtest_dep      = dependency('gtest', required : true)
gtest_main_dep = dependency('gtest_main', required : true)
libpthread_dep = meson.get_compiler('c').find_library('pthread', required : true)

# Compile-only headers (no link). Production .c files transitively pull
# in DPDK / json-c headers; we provide the include paths but never
# resolve symbols against the shared libs — --gc-sections drops the
# unused code paths first.
unit_dpdk_inc  = dependency('libdpdk', required : false).partial_dependency(
                   includes : true, compile_args : true)
unit_jsonc_inc = dependency('json-c', required : false).partial_dependency(
                   includes : true, compile_args : true)

# DEFINE_FFF_GLOBALS lives in one TU shared by all binaries.
fff_globals_lib = static_library('fff_globals',
  sources : ['common/fff_globals.cpp'],
  include_directories : unit_include_dirs,
)
fff_dep = declare_dependency(
  link_with           : fff_globals_lib,
  include_directories : unit_include_dirs,
)

# Common compile + link flags applied to every unit module.
unit_c_args = [
  '-Wall', '-Werror',
  '-include', meson.project_source_root() / 'tests/unit/common/shim_mt_log.h',
  # The shim turns dbg/info/etc. into no-ops, leaving locals that only
  # fed log macros formally unused; suppress the resulting noise.
  '-Wno-unused-variable',
  '-Wno-unused-but-set-variable',
  '-Wno-unused-function',
  '-Wno-unused-parameter',
  # --gc-sections needs per-function/data sections to discard unused code.
  '-ffunction-sections',
  '-fdata-sections',
]
unit_cpp_args = ['-Wall', '-Werror', '-Wno-pedantic']
unit_link_args = ['-Wl,--gc-sections']
unit_deps = [
  gtest_dep, gtest_main_dep, fff_dep, libpthread_dep,
  unit_dpdk_inc, unit_jsonc_inc,
]

# Module registry: { name : list-of-relative-source-paths }.
# Convention is one wrapper TU + one *_test.cpp per module, but a
# module is free to list multiple wrappers (see anc_pacing_time, which
# bridges two production files in one binary).
unit_modules = {
  'tp_stat_aggregate' : [
    'tp_stat_aggregate/wrap_st_rx_timing_parser.c',
    'tp_stat_aggregate/tp_stat_aggregate_test.cpp',
  ],
  'tp_calc_avg' : [
    'tp_calc_avg/wrap_st_rx_timing_parser.c',
    'tp_calc_avg/tp_calc_avg_test.cpp',
  ],
  'detector_fps' : [
    'detector_fps/wrap_st_rx_video_session.c',
    'detector_fps/detector_fps_test.cpp',
  ],
  'detector_dimension' : [
    'detector_dimension/wrap_st_rx_video_session.c',
    'detector_dimension/detector_dimension_test.cpp',
  ],
  'detector_n_packet' : [
    'detector_n_packet/wrap_st_rx_video_session.c',
    'detector_n_packet/detector_n_packet_test.cpp',
  ],
  'detector_packing' : [
    'detector_packing/wrap_st_rx_video_session.c',
    'detector_packing/detector_packing_test.cpp',
  ],
  'rfc4175_rtp_seq_id' : [
    'rfc4175_rtp_seq_id/wrap_st_rx_video_session.c',
    'rfc4175_rtp_seq_id/rfc4175_rtp_seq_id_test.cpp',
  ],
  'tai_from_frame_count' : [
    'tai_from_frame_count/wrap_st_tx_video_session.c',
    'tai_from_frame_count/tai_from_frame_count_test.cpp',
  ],
  'anc_pacing_time' : [
    'anc_pacing_time/wrap_st_tx_ancillary_session.c',
    'anc_pacing_time/wrap_st_tx_fastmetadata_session.c',
    'anc_pacing_time/anc_pacing_time_test.cpp',
  ],
  'transmitter_burst_fail' : [
    'transmitter_burst_fail/wrap_st_video_transmitter.c',
    'transmitter_burst_fail/transmitter_burst_fail_test.cpp',
  ],
  'frame_payload_cross_page' : [
    'frame_payload_cross_page/wrap_st_tx_video_session.c',
    'frame_payload_cross_page/frame_payload_cross_page_test.cpp',
  ],
}

foreach name, sources : unit_modules
  exe = executable('unit_' + name,
    sources             : sources,
    include_directories : unit_include_dirs,
    c_args              : unit_c_args,
    cpp_args            : unit_cpp_args,
    link_args           : unit_link_args,
    dependencies        : unit_deps,
    install             : false,
  )
  test('unit/' + name, exe, protocol : 'gtest')
endforeach
