#allow experimental api
add_global_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

#add include directory
include_dir += include_directories('include')

#add DPDK dependences
dpdk = dependency('libdpdk')

glib = dependency('glib-2.0')
gobject = dependency('gobject-2.0')

sdl2_lib = meson.get_compiler('c').find_library('SDL2')
sdl2_dep = declare_dependency(dependencies: [ sdl2_lib])


sdl2gfx_lib = meson.get_compiler('c').find_library('SDL2_gfx')
sdl2gfx_dep = declare_dependency(dependencies: [ sdl2gfx_lib])

sdl2ttf_lib = meson.get_compiler('c').find_library('SDL2_ttf')
sdl2ttf_dep = declare_dependency(dependencies: [ sdl2ttf_lib])

sdl2image_lib  = meson.get_compiler('c').find_library('SDL2_image')
sdl2image_dep  = declare_dependency(dependencies: [sdl2image_lib])

sdl2mixer_lib  = meson.get_compiler('c').find_library('SDL2_mixer')
sdl2mixer_dep  = declare_dependency(dependencies: [sdl2mixer_lib])

m_dep = meson.get_compiler('c').find_library('m', required : true)

uselocalmslib = get_option('uselocalmslib')
if uselocalmslib == false
    mediastreamer = dependency('libMediaStreamer', required : true)
else
    mediastreamer = dependency('libMediaStreamer_null', required : false)
endif

if not mediastreamer.found()
    mediastreamer_inc = include_directories('../../lib/include','../../include')
    mediastreamer_lib = 'MediaStreamer'
    mediastreamer_lib_dir = '../../build/lib'
    mediastreamer = declare_dependency(
        link_args : ['-L' + mediastreamer_lib_dir, '-l' + mediastreamer_lib],
        include_directories : mediastreamer_inc)
endif


add_project_link_arguments('-lnuma', language: 'c')

rxtx_app_sources = [
	common_app_src,
	'rxtx_app.c',
	'rx_view.c'
]

thread = ['-pthread']

#build executable
executable('RxTxApp', rxtx_app_sources,
			link_args: thread,
			include_directories : include_dir,
			dependencies: [dpdk, mediastreamer,sdl2_dep, m_dep])
