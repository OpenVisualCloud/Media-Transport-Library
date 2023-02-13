# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2022 Intel Corporation

project('mtl_test', 'cpp', default_options: ['buildtype=release'],
        version: run_command(find_program('cat', 'more'), files('../VERSION'), check: true).stdout().strip(),)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')
message('BUILD Enviroment: ' + exec_env)

cpp_c = meson.get_compiler('cpp')

# allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

mtl = dependency('mtl', required : true)
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
test_ld_args = []

# enable warning as error
test_c_args += ['-Werror']
test_c_args += ['-Wall']
test_cpp_args += ['-Werror']
test_cpp_args += ['-Wall']

if is_windows
  test_c_args += ['-DWINDOWSENV', '-D__USE_MINGW_ANSI_STDIO', '-D_WIN32_WINNT=0x0600']
  test_cpp_args += ['-DWINDOWSENV', '-D__USE_MINGW_ANSI_STDIO', '-D__STDC_FORMAT_MACROS', '-D_WIN32_WINNT=0x0600']
  libdpdklinkflags = run_command('pkg-config', '--static', '--libs', 'libdpdk')
  test_ld_args += ['-static-libstdc++','-lmingw32','-lws2_32','-lDbghelp','-lSetupapi','-ldinput8', '-ldxguid', '-ldxerr8', '-lwinmm', '-limm32','-lversion','-ldl']
endif

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address')
  test_c_args += ['-fsanitize=address']
  test_cpp_args += ['-fsanitize=address']
  asan_dep = cpp_c.find_library('asan', required : true)
endif

# build test executable
executable('KahawaiTest', sources,
  c_args : test_c_args,
  cpp_args : test_cpp_args,
  link_args: test_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, gtest, libnuma, libopenssl, libpthread]
)

# build ufd test executable
executable('KahawaiUfdTest', ufd_sources,
  c_args : test_c_args,
  cpp_args : test_cpp_args,
  link_args: test_ld_args,
  # asan should be always the first dep
  dependencies: [asan_dep, mtl, gtest, libnuma, libopenssl, libpthread]
)
