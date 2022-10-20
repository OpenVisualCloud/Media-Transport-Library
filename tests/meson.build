# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('st_test', 'cpp', default_options: ['buildtype=release'],
        version: run_command(find_program('cat', 'more'), files('../VERSION'), check: true).stdout().strip(),)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
message('BUILD Enviroment: ' + exec_env)

cpp_c = meson.get_compiler('cpp')

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

libst_dpdk = dependency('libst_dpdk', required : true)
gtest = dependency('gtest', required : true)
libpthread = cpp_c.find_library('pthread', required : true)
if not is_windows
  libnuma = cpp_c.find_library('numa', required : true)
else
  libnuma=[]
endif
libopenssl = dependency('openssl', required : true)

# add source file
subdir('src')

test_c_args = []
test_cpp_args = []

# enable warning as error
test_c_args += ['-Werror']
test_c_args += ['-Wall']
test_cpp_args += ['-Werror']
test_cpp_args += ['-Wall']

if is_windows
  test_c_args += ['-DWINDOWSENV']
  test_cpp_args += ['-DWINDOWSENV']
  test_c_args += ['-Wno-sign-compare']
  test_cpp_args += ['-Wno-sign-compare']
  test_c_args += ['-Wformat=0']
  test_cpp_args += ['-Wformat=0']
endif

test_ld_args = []

if is_windows
  libdpdklinkflags = run_command('pkg-config', '--static', '--libs', 'libdpdk')
  test_ld_args += ['-static-libstdc++','-lmingw32','-lws2_32','-lDbghelp','-lSetupapi','-ldinput8', '-ldxguid', '-ldxerr8', '-lwinmm', '-limm32','-lversion','-ljson-c','-ldl']
endif

# build test executable
executable('KahawaiTest', sources,
  c_args : test_c_args,
  cpp_args : test_cpp_args,
  link_args: test_ld_args,
  dependencies: [libst_dpdk, gtest, libnuma, libopenssl, libpthread]
)
