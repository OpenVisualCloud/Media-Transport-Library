#allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

#add DPDK dependences
dpdk = dependency('libdpdk')


cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : true)
numa_dep = cc.find_library('numa', required : true)
thread_dep = dependency('threads')

#add include directory
include_dir += include_directories('include')

#add source file
subdir('src')

#install header file (temporary, till only API functions will be used)
subdir('include')

#build static library
self_lib = static_library('MediaStreamer', sources, include_directories : include_dir, c_args : ['-mavx'], dependencies: [dpdk, m_dep, numa_dep, thread_dep], install: true)
