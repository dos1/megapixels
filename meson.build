project('millipixels', 'c', 'cpp', 'rust',
    version: '0.17.0',
    default_options: ['cpp_std=c++17']) # C++ doesn't seem to take effect
bsd = dependency('libbsd')
camera = dependency('libcamera')
gnome = import('gnome')
gtkdep = dependency('gtk+-3.0')
tiff = dependency('libtiff-4')
zbar = dependency('zbar')
threads = dependency('threads')

cc = meson.get_compiler('c')
libm = cc.find_library('m', required: false)

resources = gnome.compile_resources('megapixels-resources', 'org.postmarketos.Megapixels.gresource.xml')

conf = configuration_data()
conf.set_quoted('DATADIR', join_paths(get_option('prefix'), get_option('datadir')))
conf.set_quoted('SYSCONFDIR', get_option('sysconfdir'))
configure_file(
  output: 'config.h',
  configuration: conf )

# Define DEBUG for debug builds only (debugoptimized is not included on this one)
if get_option('buildtype') == 'debug'
  add_global_arguments('-DDEBUG', language: 'c')
endif

# Workaround for libtiff having ABI changes but not changing the internal version number
if get_option('tiffcfapattern')
  add_global_arguments('-DLIBTIFF_CFA_PATTERN', language: 'c')
endif

rust_lib = static_library('millrust',
    'src/postproc.rs',
    rust_crate_type : 'staticlib'
)

# Create a list of dependencies required by Rust.
# The dl library is required by Rust to link the library.
rust_deps = [
  meson.get_compiler('c').find_library('dl', required: false),
  dependency('threads'),
]

executable('millipixels',
           'main.c',
           'ini.c',
           'quickpreview.c',
           'camera.c',
           'device.c',
           'pipeline.c',
           'camera_config.c',
           'io_pipeline.c',
           'process_pipeline.c',
           'zbar_pipeline.c',
           'matrix.c',
           'src/libcam.cpp',
           resources,
           dependencies: [
            bsd, camera, gtkdep, libm, tiff, zbar, threads,
           ] + rust_deps,
           install : true,
           link_with: [rust_lib],
           override_options : ['cpp_std=c++17'],
)

install_data(['data/sm.puri.Millipixels.desktop'],
             install_dir : get_option('datadir') / 'applications')

install_data(['data/sm.puri.Millipixels.metainfo.xml'],
             install_dir : get_option('datadir') / 'metainfo')

install_data('data/sm.puri.Millipixels.svg',
  install_dir: join_paths(get_option('datadir'), 'icons/hicolor/scalable/apps')
)

install_data([
  'config/pine64,pinephone-1.0.ini',
  'config/pine64,pinephone-1.1.ini',
  'config/pine64,pinephone-1.2.ini',
  'config/pine64,pinetab.ini',
  'config/purism,librem5r2.ini',
  'config/purism,librem5r3.ini',
  'config/purism,librem5r4.ini',
  ],
  install_dir : get_option('datadir') / 'millipixels/config/')

install_data(['postprocess.sh'],
  install_dir : get_option('datadir') / 'millipixels/',
  install_mode: 'rwxr-xr-x')

# Tools
executable('millipixels-list-devices', 'tools/list_devices.c', 'device.c', dependencies: [gtkdep], install: true)
executable('millipixels-camera-test',
    'tools/camera_test.c',
    'camera.c',
    'device.c',
    'src/libcam.cpp',
    dependencies: [camera, gtkdep], install: true,
    override_options : ['cpp_std=c++17'],)

test_quickpreview = executable('test_quickpreview',
    'tests/test_quickpreview.c',
    'quickpreview.c',
    'camera.c',
    'matrix.c',
    'src/libcam.cpp',
    dependencies: [camera, gtkdep],
    override_options : ['cpp_std=c++17'],)
#test('quickpreview', test_quickpreview)
