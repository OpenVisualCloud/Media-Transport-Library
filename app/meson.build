# Copyright (C) 2021 Intel Corporation.
#
# This software and the related documents are Intel copyrighted materials,
# and your use of them is governed by the express license under which they
# were provided to you ("License").
# Unless the License provides otherwise, you may not use, modify, copy,
# publish, distribute, disclose or transmit this software or the related
# documents without Intel's prior written permission.
#
# This software and the related documents are provided as is, with no
# express or implied warranties, other than those that are expressly stated
# in the License.

project('Intel(R) ST DPDK sample app', 'c', default_options: ['c_std=gnu11', 'buildtype=release'], version: '0.1',)
exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
#allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

libst_dpdk = dependency('libst_dpdk', required : true)
libjson_c = dependency('json-c', required : true)
if not is_windows
libpcap = dependency('pcap', required:true)
endif
libsdl2 = dependency('sdl2', required:true)

libopenssl = dependency('openssl', required : true)

#add source file
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
app_c_args += ['-DWINDOWSENV']
app_c_args += ['-Wformat=0']
app_c_args += ['-D__USE_MINGW_ANSI_STDIO']
mingw_include_dir = mingw_root_dir + '''\include'''
windows_include_dir = '..\..\lib\windows'
app_c_args += ['-I' + mingw_include_dir]
app_c_args += ['-I' + windows_include_dir]

libdpdkcflags = run_command('pkg-config', '--cflags', 'libdpdk')
libjsonccflags = run_command('pkg-config', '--cflags', 'json-c')
libsdl2cflags = run_command('pkg-config', '--cflags', 'sdl2')
libdpdklinkflags = run_command('pkg-config', '--static', '--libs', 'libdpdk')

app_c_args += [libdpdkcflags.stdout().strip().split()]
app_c_args += [libjsonccflags.stdout().strip().split()]
app_c_args += [libsdl2cflags.stdout().strip().split()]
endif

#simd build option, enable sse4.2 default, todo: do we need AVX2/AVX512 for app ?
app_c_args += ['-msse4.2']

app_ld_args = []

app_ld_args += ['-pthread','-lm']

if is_windows
app_ld_args += ['-lmingw32','-lws2_32','-lDbghelp','-lSetupapi','-ldinput8', '-ldxguid', '-ldxerr8', '-lwinmm', '-limm32','-lversion','-lwpcap','-lPacket','-lSDL2','-lSDL2Main','-lmman','-ljson-c']
app_ld_args += ['-Wl,--whole-archive']
app_ld_args += [libdpdklinkflags.stdout().strip().split()]
app_ld_args += ['-Wl,--no-whole-archive']
endif

#build executable
executable('RxTxApp', sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk, libjson_c, libpcap, libsdl2]
)

executable('ConvApp', conv_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
)

executable('PerfRfc4175ToYuv422p10le', perf_rfc4175_to_yuv422p10le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('PerfRfc4175422be10ToLe', perf_rfc4175_422be10_to_le_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('PerfRfc4175422be10ToLe8', perf_rfc4175_422be10_to_le8_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('PerfRfc4175422be10ToV210', perf_rfc4175_422be10_to_v210_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('TxVideoSample', video_tx_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('TxRtpVideoSample', video_tx_rtp_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('RxVideoSample', video_rx_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('RxRtpVideoSample', video_rx_rtp_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('TxSt22VideoSample', video_tx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('RxSt22VideoSample', video_rx_st22_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('RxSliceVideoSample', video_rx_slice_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
)

executable('TxSliceVideoSample', video_tx_slice_sample_sources,
  c_args : app_c_args,
  link_args: app_ld_args,
  dependencies: [libst_dpdk]
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