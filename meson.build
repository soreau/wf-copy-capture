project(
    'wf-copy-capture',
    'c',
    'cpp',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>=0.51.0',
    default_options: [
        'cpp_std=c++17',
        'c_std=c11',
    ],
)

add_project_arguments(['-DWLR_USE_UNSTABLE'], language: ['cpp', 'c'])
add_project_arguments(['-DWAYFIRE_PLUGIN'], language: ['cpp', 'c'])
add_project_link_arguments(['-rdynamic','-fPIC'], language:'cpp')

wayfire = dependency('wayfire', version: '>=0.11.0')
wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols')

subdir('proto')

shared_module('copy-capture', ['copy-capture.cpp'],
    dependencies: [wayfire, wf_protos],
    install: true,
    install_dir: join_paths(get_option('libdir'), 'wayfire'))

install_data('copy-capture.xml', install_dir: wayfire.get_variable(pkgconfig: 'metadatadir'))
