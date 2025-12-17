# SPDX-License-Identifier: BSD-3-Clause

fuzz_cc = meson.get_compiler('c')
fuzz_c_args = []
fuzz_link_args = []
fuzzer_deps = []
asan_enabled = get_option('enable_asan')

if fuzz_cc.has_argument('-fsanitize=fuzzer-no-link')
  fuzz_c_args += ['-fsanitize=fuzzer-no-link']
else
  error('compiler must support -fsanitize=fuzzer-no-link to build fuzzers')
endif

if fuzz_cc.has_link_argument('-fsanitize=fuzzer')
  fuzz_link_args += ['-fsanitize=fuzzer']
else
  fuzzer_lib = fuzz_cc.find_library('Fuzzer', required : false)
  if fuzzer_lib.found()
    fuzzer_deps += [fuzzer_lib]
  else
    error('libFuzzer support (-fsanitize=fuzzer or -lFuzzer) is required to build fuzzers')
  endif
endif

if asan_enabled
  if not fuzz_cc.has_argument('-fsanitize=address')
    error('enable_asan=true requires compiler support for -fsanitize=address')
  endif
  fuzz_c_args += ['-fsanitize=address', '-DMTL_HAS_ASAN']
  fuzz_link_args += ['-fsanitize=address']

  if fuzz_cc.get_id() != 'clang'
    fuzzer_deps += [fuzz_cc.find_library('asan', required : true)]
  endif
endif

fuzz_include_dirs = [
  include_directories('../../include'),
  include_directories('../../lib/src'),
]

fuzz_targets = [
  ['st40_rx_rtp_fuzz', 'st40/st40_rx_rtp_fuzz.c'],
  ['st40_ancillary_helpers_fuzz', 'st40/st40_ancillary_helpers_fuzz.c'],
  ['st30_rx_frame_fuzz', 'st30/st30_rx_frame_fuzz.c'],
  ['st20_rx_frame_fuzz', 'st20/st20_rx_frame_fuzz.c'],
  ['st22_rx_frame_fuzz', 'st22/st22_rx_frame_fuzz.c'],
]

if host_machine.cpu_family() == 'x86' or host_machine.cpu_family() == 'x86_64'
  # DPDK memcpy helpers rely on SSSE3 intrinsics; explicitly enable them for clang builds
  fuzz_c_args += ['-mssse3']
  fuzz_link_args += ['-mssse3']
endif

stdcpp_dep = fuzz_cc.find_library('stdc++', required : true)

foreach target : fuzz_targets
  exe_name = target[0]
  src = target[1]
  deps = [mtl_internal_dep, dpdk_dep, libm_dep, stdcpp_dep] + fuzzer_deps
  executable(
    exe_name,
    src,
    include_directories : fuzz_include_dirs,
    c_args : fuzz_c_args,
    link_args : fuzz_link_args,
    dependencies : deps,
    install : false)
endforeach
