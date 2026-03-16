wl_protocol_dir = wayland_protos.get_pkgconfig_variable('pkgdatadir')

wayland_scanner = find_program('wayland-scanner')

wayland_scanner_code = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_server = generator(
	wayland_scanner,
	output: '@BASENAME@-server-protocol.h',
	arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

server_protocols = [
    join_paths(wl_protocol_dir, 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml'),
]

wl_protos_src = []
wl_protos_headers = []

foreach p : server_protocols
	xml = join_paths(p)
	wl_protos_headers += wayland_scanner_server.process(xml)
	wl_protos_src += wayland_scanner_code.process(xml)
endforeach

lib_wl_protos = static_library('wl_protos', wl_protos_src + wl_protos_headers)

wf_protos = declare_dependency(
	link_with: lib_wl_protos,
	sources: wl_protos_headers,
)
