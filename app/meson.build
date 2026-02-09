# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('mtl_app', 'c', default_options: ['buildtype=release'],
  version: run_command(find_program('cat'), files('../VERSION'), check: true).stdout().strip(),)

gpu_direct = dependency('mtl_gpu_direct', required: false)
if gpu_direct.found()
  add_global_arguments('-DMTL_GPU_DIRECT_ENABLED', language : 'c')
endif

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
libpcap = dependency('pcap', required: true)

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

libopenssl = dependency('openssl', required : false)
if libopenssl.found()
  add_global_arguments('-DAPP_HAS_SSL', language : 'c')
else
  message('openssl not found')
endif

# add source file
subdir('tools')
subdir('sample')
subdir('perf')
subdir('udp')
subdir('v4l2_to_ip')

app_c_args = []
app_ld_args = []

# enable warning as error for non debug build
if get_option('buildtype') != 'debug'
  app_c_args += ['-Werror']
endif
app_c_args += ['-Wall']
app_c_args += ['-Wunused-parameter']
app_c_args += ['-I' + join_paths(meson.current_source_dir(), '..', 'include')]
app_c_args += ['-I' + join_paths(meson.current_build_dir(), '..')]
app_c_args += ['-I' + join_paths(meson.project_build_root(), 'include')]

if is_windows
  app_c_args += ['-DWINDOWSENV']
  windows_include_dir = '..\..\lib\windows'
  app_c_args += ['-I' + windows_include_dir]
endif

# simd build option, enable sse4.2 default, todo: do we need AVX2/AVX512 for app ?
app_c_args += ['-msse4.2']

if is_windows
  ws2_32_dep = cc.find_library('ws2_32', required: true)
  mman_dep = cc.find_library('mman', required: true)
else
  ws2_32_dep = [] # add this when the code uses hton/ntoh
  mman_dep = [] # add this when the code uses mmap/munmap
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

# Color convert tool for raw yuv file
executable('ConvApp', conv_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl]
)

# Legacy lcore manager
executable('LcoreMgr', lcore_mgr_sources,
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
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfP10LeToRfc4175422be10', perf_p10le_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfRfc4175422be10ToP16Le', perf_rfc4175_422be10_to_p16le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfP16LeToRfc4175422be10', perf_p16le_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)


executable('PerfRfc4175422be10ToLe', perf_rfc4175_422be10_to_le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfRfc4175422le10ToBe', perf_rfc4175_422le10_to_be_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfRfc4175422be10ToLe8', perf_rfc4175_422be10_to_le8_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfRfc4175422be10ToV210', perf_rfc4175_422be10_to_v210_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfV210ToRfc4175422be10', perf_v210_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfRfc4175422be10ToY210', perf_rfc4175_422be10_to_y210_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfY210ToRfc4175422be10', perf_y210_to_rfc4175_422be10_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfRfc4175422be12ToLe', perf_rfc4175_422be12_to_le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfRfc4175422be12ToP12Le', perf_rfc4175_422be12_to_p12le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('PerfRfc4175422be10ToP8', perf_rfc4175_422be10_to_p8_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

# Pipeline video samples app
executable('TxSt20PipelineSample', pipeline_tx_st20_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('RxSt20PipelineSample', pipeline_rx_st20_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('TxSt22PipelineSample', pipeline_tx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('RxSt22PipelineSample', pipeline_rx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('TxSt30PipelineSample', pipeline_tx_st30_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)
executable('TxSt40PipelineSample', pipeline_tx_st40_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)
executable('RxSt30PipelineSample', pipeline_rx_st30_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)
executable('RxSt40PipelineSample', pipeline_rx_st40_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

# Video forward samples app
executable('RxSt20pTxSt22pFwd', rx_st20p_tx_st22p_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('RxSt20pTxSt20pFwd', rx_st20p_tx_st20p_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('RxSt20pTxSt20pDownsampleFwd', rx_st20p_tx_st20p_downsample_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('RxSt20pTxSt20pDownsampleMergeFwd', rx_st20p_tx_st20p_downsample_merge_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
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
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('RxSt20pTxSt20pMergeFwd', rx_st20p_tx_st20p_merge_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

# Misc video samples app
executable('RxVideoTimingParserSample', rx_st20p_timing_parser_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('RxVideoAutoDetectSample', rx_st20p_auto_detect_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

# Legacy video samples app
executable('TxVideoSample', video_tx_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('RxVideoSample', video_rx_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('TxSt22VideoSample', video_tx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('RxSt22VideoSample', video_rx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('RxSt20TxSt20Fwd', rx_st20_tx_st20_fwd_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

# Ext frame samples app
executable('TxSt20pExtFrameSample', pipeline_tx_st20_ext_frame_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('RxSt20pDynExtFrameSample', pipeline_rx_st20_dyn_ext_frame_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('RxSt20pHdrSplitGpuDirect', pipeline_rx_st20_dyn_hdr_split_gpu_direct_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('TxVideoSplitSample', video_tx_split_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, ws2_32_dep, mman_dep]
)

# Low level video samples app
executable('TxRtpVideoSample', video_tx_rtp_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('RxRtpVideoSample', video_rx_rtp_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('TxSliceVideoSample', video_tx_slice_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('RxSliceVideoSample', video_rx_slice_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

# experimental
executable('RxSt20CombinedRedundantSample', redundant_rx_st20_combined_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

# New unified API samples
executable('NewApiTxVideoLibOwned', new_api_tx_video_lib_owned_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('NewApiRxVideoLibOwned', new_api_rx_video_lib_owned_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('NewApiTxVideoUserOwned', new_api_tx_video_user_owned_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

executable('NewApiRxVideoUserOwned', new_api_rx_video_user_owned_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep, mman_dep]
)

# Dma sample app
executable('DmaSample', dma_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libopenssl, ws2_32_dep]
)

# Dma performance benchmark
executable('PerfDma', perf_dma_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, ws2_32_dep]
)

# UDP sample app
executable('UdpServerSample', upd_server_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('UdpClientSample', upd_client_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('UfdServerSample', ufd_server_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
)

executable('UfdClientSample', ufd_client_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libpthread, ws2_32_dep]
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

# v4l2 to IP sample app
if app_has_sdl2 and not is_windows
executable('V4l2toIPApp', v4l2_to_ip_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, libsdl2, libsdl2_ttf, libm, libpthread]
)
endif

if gpu_direct.found()
  executable('GpuDirectVideoTxMultiSample', gpu_direct_tx_sample_sources,
    c_args : app_c_args,
    link_args: app_ld_args,
    dependencies: [asan_dep, libpthread, mtl, gpu_direct]
  )
  executable('GpuDirectVideoRxMultiSample', gpu_direct_rx_sample_sources,
    c_args : app_c_args,
    link_args: app_ld_args,
    dependencies: [asan_dep, libpthread, mtl, gpu_direct, libsdl2]
  )
endif
