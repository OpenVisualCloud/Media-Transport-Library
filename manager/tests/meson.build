# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2023 Intel Corporation

# The manager unit tests. They need no NIC, no root and no libxdp: every device
# call goes through a fake. Run them with `meson test -C <builddir>`.

if not get_option('enable_tests')
  message('manager unit tests off')
  subdir_done()
endif

gtest_dep = dependency('gtest', required: false)
gtest_main_dep = dependency('gtest_main', required: false)

if not gtest_dep.found() or not gtest_main_dep.found()
  message('gtest not found, no manager unit tests')
  subdir_done()
endif

test_sources = files(
  'test_abuse.cpp',
  'test_client_wire.cpp',
  'test_instance.cpp',
  'test_interface.cpp',
  'test_lcore.cpp',
  'test_logging.cpp',
  'test_server.cpp',
  'test_sock_path.cpp',
)

# The test cases run a server in a thread, so the binary needs pthread.
thread_dep = dependency('threads')

mtlm_test = executable('MtlManagerTest', test_sources,
  cpp_args: cpp_args,
  link_args: link_cpp_args,
  include_directories: include_directories('.', '..'),
  dependencies: [asan_dep, mtlm_core_dep, gtest_dep, gtest_main_dep, thread_dep],
)

# An asan build links libasan after the other libraries, and the runtime then
# refuses to start. Preload it here, as ./build.sh does for tests/unit. The probe
# test gets the path and not the preload: a preload of the whole script also
# puts bash under LeakSanitizer, and bash leaks at exit.
test_env = {}
probe_env = {'MTLM_BUILD_DIR': meson.current_build_dir() / '..'}
if get_option('enable_asan') == true
  asan_lib = run_command(cpp_c.cmd_array()[0], '-print-file-name=libasan.so',
    check: true).stdout().strip()
  test_env = {'LD_PRELOAD': asan_lib}
  probe_env += {'MTLM_PRELOAD': asan_lib}
endif

test('manager', mtlm_test,
  protocol: 'gtest',
  env: test_env,
  timeout: 120,
)

# MtlManagerProbe against a manager of its own. It is a test of the public API
# and not a unit test, so it runs the real daemon, but with no interface named,
# which is the part that needs neither a NIC nor root. Name an interface by hand
# for the rest:
#   manager/tools/run_probe.sh --ifname <name>
test('manager_probe', files('../tools/run_probe.sh'),
  depends: [mtlm_probe, mtlm_manager_exe],
  env: probe_env,
  timeout: 180,
)
