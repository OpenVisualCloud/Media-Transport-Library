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

project('Intel(R) ST DPDK tests', 'cpp', default_options: ['cpp_std=gnu++11', 'buildtype=release'], version: '0.1',)

cpp_c = meson.get_compiler('cpp')

#allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

libst_dpdk = dependency('libst_dpdk', required : true)
gtest = dependency('gtest', required : true)
libnuma = cpp_c.find_library('numa', required : true)
libopenssl = dependency('openssl', required : true)

#add source file
subdir('src')

test_c_args = []
test_cpp_args = []

#enable warning as error
test_c_args += ['-Werror']
test_c_args += ['-Wall']
test_cpp_args += ['-Werror']
test_cpp_args += ['-Wall']

test_ld_args = []

test_ld_args += ['-pthread']

#build executable
executable('KahawaiTest', sources,
  c_args : test_c_args,
  cpp_args : test_cpp_args,
  link_args: test_ld_args,
  dependencies: [libst_dpdk, gtest, libnuma, libopenssl]
)