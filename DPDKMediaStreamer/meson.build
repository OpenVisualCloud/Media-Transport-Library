project('DPDK Media Streamer', 'c',
                default_options: ['c_std=gnu11', 'buildtype=release'],
		version: '1.0',)

#enable BSD_SOURCE
add_global_arguments('-D_DEFAULT_SOURCE', language : 'c')

#get external variables
add_global_arguments('-DGIT="'+run_command('git', 'describe', '--always').stdout().strip()+'"', language : 'c')
add_global_arguments('-DBUILD="'+run_command('printenv', 'BUILD_ID').stdout().strip()+'"', language : 'c')

self_lib = []

#add include directory
include_dir = [
	include_directories('include')
]

#install header files
subdir('include')

#build library
subdir('lib')

pkg = import('pkgconfig')
pkg.generate(libraries : self_lib,
             version : '1.0',
             name : 'MediaStreamer',
             filebase : 'libMediaStreamer',
             description : 'DPDK based Media Streamer')
