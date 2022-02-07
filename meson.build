project('mrss', 'c',
	default_options: [
		'c_std=c11',
		'warning_level=2',
	]
)

git_describe = 'git describe --always --tags --dirty --match v*'.split(' ')

executable('mrss',
	'mrss.c',
	'xml_utils.c',
	'sha1.c',
	'atom.c',
	'rdf.c',
	'rss.c',
	vcs_tag(
		command: git_describe,
		input: 'version.h.in',
		output: 'version.h',
	),
	dependencies: [
		dependency('libcurl'),
		dependency('libxml2'),
	],
	install: true,
)

install_man('mrss.1')

test('all checks', find_program('test/check'),
	env: [
		'BUILD_ROOT=' + meson.build_root(),
		'WORK_ROOT=' + meson.build_root() / 'test',
		'TEST_ROOT=' + meson.source_root() / 'test',
	],
)
