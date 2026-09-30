# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

project('mtl_manager', ['c', 'cpp'],
  # gnu11, not c11: the client library calls gethostname(), which glibc hides
  # behind the GNU feature macros.
  default_options: ['buildtype=release', 'cpp_std=c++17', 'c_std=gnu11'],
  version: run_command(find_program('cat'), files('../VERSION'), check: true).stdout().strip(),
)

exec_env = host_machine.system()
set_variable('is_windows', exec_env == 'windows')

if is_windows
  message('not supported on Windows')
  subdir_done()
endif

message('BUILD Environment: ' + exec_env)

cc = meson.get_compiler('c')
cpp_c = meson.get_compiler('cpp')

# The client library and the daemon share mtlm_sock_path.c, so the two can
# never disagree about which socket to use.
client_sources = files('mtlm_client.c', 'mtlm_sock_path.c')

# Everything the daemon does, apart from main(). The unit tests link the same
# objects, so a test covers the code that ships.
core_sources = files(
  'logging.cpp',
  'mtl_instance.cpp',
  'mtl_interface.cpp',
  'mtl_lcore.cpp',
  'mtlm_netdev_linux.cpp',
  'mtlm_server.cpp',
  'mtlm_xdp.cpp',
)

# The public API of the manager is part of the public API of MTL, so the two
# headers live in ../include with mtl_api.h and the rest. The library project
# installs them as well; installing the same two files from here keeps the
# manager buildable and installable on its own.
public_headers = files('../include/mtlm_api.h', '../include/mtl_mproto.h')

# '.' for the private headers of the daemon, '../include' for the two public
# ones. Every target here needs both.
mtlm_include_dir = include_directories('.', '../include')

c_args = ['-Wall']
cpp_args = ['-Wall']
link_cpp_args = []

if get_option('buildtype') != 'debug'
  c_args += ['-Werror']
  cpp_args += ['-Werror']
  # enable error on  Variable Length Arrays
  c_args += ['-Wvla']
  cpp_args += ['-Wvla']
endif

# default no asan dep
asan_dep = []
if get_option('enable_asan') == true
  message('Enable -fsanitize=address')
  c_args += ['-fsanitize=address']
  cpp_args += ['-fsanitize=address']
  asan_dep = cpp_c.find_library('asan', required : true)
endif

mtlm_conf = configuration_data()
# get external variables
add_global_arguments('-D__MTLM_GIT__="'+ run_command('git', 'describe', '--abbrev=8', '--dirty', '--always', check: false).stdout().strip() + '"', language : 'cpp')
# parse mtlm config
# parse build config
prj_ver = meson.project_version().split('.')
mtlm_conf.set('MTLM_VERSION_MAJOR', prj_ver.get(0).to_int())
mtlm_conf.set('MTLM_VERSION_MINOR', prj_ver.get(1).to_int())
mtlm_conf.set('MTLM_VERSION_LAST', prj_ver.get(2).to_int())
mtlm_conf.set_quoted('MTLM_VERSION_EXTRA', prj_ver.get(3))
# parse compiler config
cpp_c_ver = cpp_c.get_id() + '-' + cpp_c.version()
mtlm_conf.set_quoted('MTLM_COMPILER', cpp_c_ver)
# build config file
build_cfg = 'mtlm_build_config.h'
configure_file(output: build_cfg, configuration: mtlm_conf)

# xdp check
libxdp_dep = dependency('libxdp', required: false)
libbpf_dep = dependency('libbpf', required: false)
if libxdp_dep.found() and libbpf_dep.found()
  add_global_arguments('-DMTL_HAS_XDP_BACKEND', language : 'cpp')
  clang = find_program('clang')
  llvm_strip = find_program('llvm-strip')
  multiarch = run_command(find_program('cc'), '-dumpmachine', check: true).stdout().strip()
  # Build XDP prog obj
  xdp_src = files('mtl.xdp.c')
  xdp_temp_obj = custom_target('mtl.xdp.temp.o',
    input : xdp_src,
    output : 'mtl.xdp.temp.o',
    command : [clang, '-g', '-O2', '-target', 'bpf', '-I/usr/include/' + multiarch,
      '-c', '@INPUT@', '-o', '@OUTPUT@']
  )
  xdp_obj = custom_target('mtl.xdp.o',
    depends : xdp_temp_obj,
    input : xdp_temp_obj,
    output : 'mtl.xdp.o',
    install: true,
    install_dir: get_option('prefix') + '/lib/bpf',
    command : [llvm_strip, '-g', '@INPUT@', '-o', '@OUTPUT@']
  )
else
  message('libxdp and libbpf not found, no af_xdp backend')
endif

# The client library. The MTL library links it for the instance IPC, and a test
# or a third party tool can link it too. It needs no NIC and no privilege.
# A shared library version holds three numbers only, and the project version has
# a fourth part, so the numbers come from the split above.
client_lib_version = '@0@.@1@.@2@'.format(prj_ver.get(0).to_int(),
  prj_ver.get(1).to_int(), prj_ver.get(2).to_int())

mtlm_client_lib = library('mtlm_client', client_sources,
  c_args: c_args,
  include_directories: mtlm_include_dir,
  version: client_lib_version,
  soversion: prj_ver.get(0).to_int(),
  install: true,
  dependencies: [asan_dep],
)

install_headers(public_headers, subdir: 'mtl')

pkg = import('pkgconfig')
pkg.generate(mtlm_client_lib,
  name: 'mtlm_client',
  filebase: 'mtlm_client',
  description: 'Client of the MTL Manager control socket',
  # Both forms, the same way mtl.pc does it: '.' so <mtl/mtlm_api.h> resolves
  # from these flags alone, and 'mtl' so <mtlm_api.h> works too. Without '.' the
  # first form would only build where the compiler already searches the prefix.
  subdirs: ['.', 'mtl'],
)

mtlm_client_dep = declare_dependency(
  link_with: mtlm_client_lib,
  include_directories: mtlm_include_dir,
)

# The daemon logic, without main(), so the unit tests can link it.
mtlm_core_lib = static_library('mtlm_core', core_sources,
  cpp_args: cpp_args,
  include_directories: mtlm_include_dir,
  dependencies: [asan_dep, libxdp_dep, libbpf_dep],
)

mtlm_core_dep = declare_dependency(
  link_with: mtlm_core_lib,
  include_directories: mtlm_include_dir,
  dependencies: [mtlm_client_dep, libxdp_dep, libbpf_dep],
)

mtlm_manager_exe = executable('MtlManager', files('mtl_manager.cpp'),
  cpp_args: cpp_args,
  link_args: link_cpp_args,
  install: true,
  dependencies: [asan_dep, mtlm_core_dep],
)

subdir('tools')
subdir('tests')
subdir('service')
