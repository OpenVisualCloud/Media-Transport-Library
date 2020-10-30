project('DPDK MediaStreamer App Sample', 'c',
		default_options: ['c_std=gnu99', 'buildtype=release'],
		version: '1.0',)

#add DPDK dependences
dpdk = dependency('libdpdk')

#add MediaStreamer dependency
mediastreamer_inc = include_directories('../inc')
mediastreamer_lib = 'MediaStreamer'
mediastreamer_lib_dir = '../lib'
mediastreamer = declare_dependency(
		link_args : ['-L' + mediastreamer_lib_dir, '-l' + mediastreamer_lib],
		include_directories : mediastreamer_inc)

rxtx_app_sources = [
	'src/common/send_app.c',
	'src/common/recv_app.c',
	'src/rxtx_app.c'
]

add_project_link_arguments('-lnuma', language: 'c')

thread = ['-pthread']

#build executable
executable('RxTxApp', rxtx_app_sources,
			link_args: thread,
			include_directories : mediastreamer_inc,
			dependencies: [dpdk, mediastreamer])