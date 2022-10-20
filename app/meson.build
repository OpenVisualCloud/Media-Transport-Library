# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('Intel(R) ST DPDK sample app', 'c', default_options: ['buildtype=release'], version: '22.12',)

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

#detect os
exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

cc = meson.get_compiler('c')

libst_dpdk = dependency('libst_dpdk', required : true)
libm = cc.find_library('m', required : true)
libpthread = cc.find_library('pthread', required : true)
libjson_c = dependency('json-c', required : true)
if not is_windows
  libpcap = dependency('pcap', required: true)
endif
libsdl2 = dependency('sdl2', required: true)
libsdl2_ttf = dependency('SDL2_ttf', required: false)
if libsdl2_ttf.found()
  add_global_arguments('-DAPP_HAS_SDL2_TTF', language : 'c')
else
  message('SDL2_ttf not found')
endif
libopenssl = dependency('openssl', required : true)

# add source file
subdir('src')
subdir('tools')
subdir('sample')
subdir('perf')

app_c_args = []

#enable warning as error
app_c_args += ['-Werror']
app_c_args += ['-Wall']

if is_windows
  libpcap =[]
  mingw_root_dir = 'c:\mingw64'
  app_c_args += ['-DWINDOWSENV','-D_WIN32_WINNT=0x0600']
  app_c_args += ['-Wformat=0']
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

app_ld_args = []

if is_windows
  app_ld_args += ['-lmingw32','-lws2_32','-lDbghelp','-lSetupapi','-ldinput8', '-ldxguid', '-ldxerr8', '-lwinmm', '-limm32','-lversion','-lwpcap','-lPacket','-lSDL2','-lSDL2Main','-lmman','-ljson-c','-ldl']
endif

# build executable
executable('RxTxApp', sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libjson_c, libpcap, libsdl2, libsdl2_ttf, libm, libpthread]
)

executable('ConvApp', conv_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, ]
)

executable('PerfRfc4175422be10ToP10Le', perf_rfc4175_422be10_to_p10le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('PerfP10LeToRfc4175422be10', perf_p10le_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('PerfRfc4175422be10ToLe', perf_rfc4175_422be10_to_le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('PerfRfc4175422le10ToBe', perf_rfc4175_422le10_to_be_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('PerfRfc4175422be10ToLe8', perf_rfc4175_422be10_to_le8_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('PerfRfc4175422be10ToV210', perf_rfc4175_422be10_to_v210_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('PerfV210ToRfc4175422be10', perf_v210_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('PerfRfc4175422be10ToY210', perf_rfc4175_422be10_to_y210_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('PerfY210ToRfc4175422be10', perf_y210_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('TxVideoSample', video_tx_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('TxRtpVideoSample', video_tx_rtp_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxVideoSample', video_rx_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxRtpVideoSample', video_rx_rtp_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('TxSt22VideoSample', video_tx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxSt22VideoSample', video_rx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('TxSt22PipelineSample', pipeline_tx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxSt22PipelineSample', pipeline_rx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxSliceVideoSample', video_rx_slice_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('TxSliceVideoSample', video_tx_slice_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxSt20pTxSt22pFwd', rx_st20p_tx_st22p_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxSt20pTxSt20pFwd', rx_st20p_tx_st20p_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxSt20TxSt20Fwd', rx_st20_tx_st20_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('TxSt20PipelineSample', pipeline_tx_st20_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxSt20PipelineSample', pipeline_rx_st20_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('RxSt20RedundantSample', redundant_rx_st20_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libpthread]
)

executable('DmaSample', dma_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libopenssl]
)

executable('PerfDma', perf_dma_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)
