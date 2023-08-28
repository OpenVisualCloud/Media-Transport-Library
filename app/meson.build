# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('mtl_app', 'c', default_options: ['buildtype=release'],
  version: run_command(find_program('cat', 'more'), files('../VERSION'), check: true).stdout().strip(),)

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

# detect os
exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

cc = meson.get_compiler('c')
toolchain = cc.get_id()

mtl = dependency('mtl', required : true)
libm = cc.find_library('m', required : true)
libpthread = cc.find_library('pthread', required : true)
libjson_c = dependency('json-c', required : true)
if not is_windows
  libpcap = dependency('pcap', required: true)
endif
libsdl2 = dependency('sdl2', required: false)
if libsdl2.found()
  add_global_arguments('-DAPP_HAS_SDL2', language : 'c')
  set_variable('app_has_sdl2', true)
else
  message('SDL2 not found')
  set_variable('app_has_sdl2', false)
endif
libsdl2_ttf = dependency('SDL2_ttf', required: false)
if libsdl2_ttf.found()
  add_global_arguments('-DAPP_HAS_SDL2_TTF', language : 'c')
else
  message('SDL2_ttf not found')
endif
libopenssl = dependency('openssl', required : true)
dpdk_dep = dependency('libdpdk', required : true)

# add source file
subdir('src')
subdir('tools')
subdir('sample')
subdir('perf')
subdir('udp')

app_c_args = []
app_ld_args = []

# enable warning as error for non debug build
if get_option('buildtype') != 'debug'
  app_c_args += ['-Werror']
endif
app_c_args += ['-Wall']

if is_windows
  libpcap =[]
  mingw_root_dir = 'c:\mingw64'
  app_c_args += ['-DWINDOWSENV','-D_WIN32_WINNT=0x0600']
  app_c_args += ['-D__USE_MINGW_ANSI_STDIO']
  mingw_include_dir = mingw_root_dir + '''\include'''
  windows_include_dir = '..\..\lib\windows'
  app_c_args += ['-I' + mingw_include_dir]
  app_c_args += ['-I' + windows_include_dir]

  libdpdkcflags = run_command('pkg-config', '--cflags', 'libdpdk')
  libjsonccflags = run_command('pkg-config', '--cflags', 'json-c')
  libsdl2cflags = run_command('pkg-config', '--cflags', 'sdl2')
  libsdl2_ttfcflags = run_command('pkg-config', '--cflags', 'sdl2-ttf')
  libopensslcflags = run_command('pkg-config', '--cflags', 'openssl')

  app_c_args += [libdpdkcflags.stdout().strip().split()]
  app_c_args += [libjsonccflags.stdout().strip().split()]
  app_c_args += [libsdl2cflags.stdout().strip().split()]
  app_c_args += [libsdl2_ttfcflags.stdout().strip().split()]
  app_c_args += [libopensslcflags.stdout().strip().split()]
endif

# simd build option, enable sse4.2 default, todo: do we need AVX2/AVX512 for app ?
app_c_args += ['-msse4.2']

if is_windows
  app_ld_args += ['-lmingw32','-lws2_32','-lDbghelp','-lSetupapi','-ldinput8', '-ldxguid', '-ldxerr8', '-lwinmm', '-limm32','-lversion','-lwpcap','-lPacket','-lSDL2','-lSDL2Main','-lmman','-ljson-c','-ldl']
endif

mtl_has_RxSt20TxSt20SplitFwd = true
if (toolchain == 'clang' and cc.version().version_compare('<9.0.0'))
  message('Disable RxSt20TxSt20SplitFwd as no stdatomic for this clang version')
  mtl_has_RxSt20TxSt20SplitFwd = false
endif
if is_windows
  message('Disable RxSt20TxSt20SplitFwd as no <sys/queue.h> for windows')
  mtl_has_RxSt20TxSt20SplitFwd = false
endif

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address for app')
  app_c_args += ['-fsanitize=address']
  asan_dep = cc.find_library('asan', required : true)
endif

# build executable
executable('RxTxApp', sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libjson_c, libpcap, libsdl2, libsdl2_ttf, libm, libpthread]
)

# Color convert tool for raw yuv file
executable('ConvApp', conv_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl]
)

# Performance benchmarks for color convert
executable('PerfRfc4175422be10ToP10Le', perf_rfc4175_422be10_to_p10le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfP10LeToRfc4175422be10', perf_p10le_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfRfc4175422be10ToLe', perf_rfc4175_422be10_to_le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfRfc4175422le10ToBe', perf_rfc4175_422le10_to_be_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfRfc4175422be10ToLe8', perf_rfc4175_422be10_to_le8_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfRfc4175422be10ToV210', perf_rfc4175_422be10_to_v210_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfV210ToRfc4175422be10', perf_v210_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfRfc4175422be10ToY210', perf_rfc4175_422be10_to_y210_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfY210ToRfc4175422be10', perf_y210_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfRfc4175422be12ToLe', perf_rfc4175_422be12_to_le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('PerfRfc4175422be12ToP12Le', perf_rfc4175_422be12_to_p12le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

# Pipeline video samples app
executable('TxSt20PipelineSample', pipeline_tx_st20_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt20PipelineSample', pipeline_rx_st20_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('TxSt22PipelineSample', pipeline_tx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt22PipelineSample', pipeline_rx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

# Video forward samples app
executable('RxSt20pTxSt22pFwd', rx_st20p_tx_st22p_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt20pTxSt20pFwd', rx_st20p_tx_st20p_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt20pTxSt20pDownsampleFwd', rx_st20p_tx_st20p_downsample_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt20pTxSt20pDownsampleMergeFwd', rx_st20p_tx_st20p_downsample_merge_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

if mtl_has_RxSt20TxSt20SplitFwd
executable('RxSt20TxSt20SplitFwd', rx_st20_tx_st20_split_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl]
)
endif

executable('RxSt20pTxSt20pSplitFwd', rx_st20p_tx_st20p_split_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt20pTxSt20pMergeFwd', rx_st20p_tx_st20p_merge_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

# Misc video samples app
executable('TxVideoSplitSample', video_tx_split_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl]
)

executable('RxSt20RedundantSample', redundant_rx_st20_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

# Legacy video samples app
executable('TxVideoSample', video_tx_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxVideoSample', video_rx_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('TxSt22VideoSample', video_tx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt22VideoSample', video_rx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt20TxSt20Fwd', rx_st20_tx_st20_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

# Ext frame samples app
executable('TxSt20pExtFrameSample', pipeline_tx_st20_ext_frame_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt20pDynExtFrameSample', pipeline_rx_st20_dyn_ext_frame_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSt20pHdrSplitGpuDirect', pipeline_rx_st20_dyn_hdr_split_gpu_direct_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

# Low level video samples app
executable('TxRtpVideoSample', video_tx_rtp_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxRtpVideoSample', video_rx_rtp_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('TxSliceVideoSample', video_tx_slice_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('RxSliceVideoSample', video_rx_slice_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

# Dma sample app
executable('DmaSample', dma_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libopenssl]
)

# Dma performance benchmark
executable('PerfDma', perf_dma_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl]
)

# UDP sample app
executable('UdpServerSample', upd_server_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('UdpClientSample', upd_client_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('UfdServerSample', ufd_server_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('UfdClientSample', ufd_client_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

# WA: no socket/poll API for windows
if not is_windows
executable('UsocketServerSample', usocket_server_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)

executable('UsocketClientSample', usocket_client_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread]
)
endif

if not is_windows
# v4l2 to IP sample app
executable('V4l2toIPApp', v4l2_to_ip_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libsdl2, libsdl2_ttf, libm, libpthread]
)
endif