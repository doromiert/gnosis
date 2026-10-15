fs = import('fs')
cef_root = get_option('cef_root')
ublock_dir = get_option('ublock_dir')
if cef_root == '' or ublock_dir == ''
  error('Set cef_root to the pinned CEF SDK and ublock_dir to official full uBlock Origin; nix develop supplies both.')
endif
# ICU is located relative to the loaded libcef.so, including development builds.
foreach name : ['libcef.so', 'libEGL.so', 'libGLESv2.so', 'snapshot_blob.bin', 'v8_context_snapshot.bin', 'chrome-sandbox']
  if fs.exists(cef_root / 'Release' / name)
    run_command('ln', '-sfn', cef_root / 'Release' / name, meson.current_build_dir() / name, check: true)
  endif
endforeach
foreach name : ['icudtl.dat', 'chrome_100_percent.pak', 'chrome_200_percent.pak', 'resources.pak', 'locales']
  if fs.exists(cef_root / 'Resources' / name)
    run_command('ln', '-sfn', cef_root / 'Resources' / name, meson.current_build_dir() / name, check: true)
  endif
endforeach
cpp = meson.get_compiler('cpp')
cef_dep = cpp.find_library('cef', dirs: meson.current_build_dir())
if get_option('cef_wrapper') != ''
  wrapper_dep = declare_dependency(link_args: get_option('cef_wrapper'))
else
  cmake = import('cmake')
  wrapper_options = cmake.subproject_options()
  wrapper_options.add_cmake_defines({'CEF_ROOT': cef_root})
  wrapper_project = cmake.subproject('cef-wrapper', options: wrapper_options)
  wrapper_dep = wrapper_project.dependency('libcef_dll_wrapper')
endif
engine_resource = gnome.compile_resources('jupiter-engine-resources',
  'data/engine.gresource.xml', c_name: 'jupiter_engine', source_dir: 'data')
engine_includes = include_directories('.', 'select', cef_root)
bridge_dir = developer_mode ? meson.current_source_dir() / 'data/tab-bridge' : pkgdatadir / 'tab-bridge'
engine = static_library('jupiter-engine',
  ['ephy-engine-origin.cpp', 'ephy-engine-favicon.c', 'ephy-engine-view.cpp', 'ephy-engine-scheme.cpp', 'ephy-engine-print.cpp', 'select/application.cpp', 'select/browser.cpp', 'select/clipboard.cpp',
   'select/cursor.cpp', 'select/policy.cpp', 'select/extensions.cpp', 'select/extension-host.cpp', engine_resource],
  include_directories: engine_includes,
  dependencies: [gtk_dep, libadwaita_dep, json_glib_dep, webkitgtk_dep, dependency('epoxy'), cef_dep, wrapper_dep],
  cpp_args: ['-DCEF_RESOURCE_DIR="' + cef_root / 'Resources' + '"',
    '-DJUPITER_UBLOCK_DIR="' + ublock_dir + '"',
    '-DJUPITER_BRIDGE_DIR="' + bridge_dir + '"'],
  pic: true)
ephyengine_dep = declare_dependency(link_with: engine, include_directories: include_directories('.'), dependencies: [cef_dep, wrapper_dep])
install_subdir(cef_root / 'Resources', install_dir: pkglibdir / 'cef', strip_directory: true)
install_subdir(cef_root / 'Release', install_dir: pkglibdir / 'cef', strip_directory: true)
install_subdir('data/tab-bridge', install_dir: pkgdatadir)
install_data(cef_root / 'LICENSE.txt', cef_root / 'CREDITS.html', install_dir: datadir / 'licenses/jupiter-cef')
if get_option('unit_tests').enabled()
  favicon_test = executable('engine-favicon-test', ['tests/favicon-test.c', 'ephy-engine-favicon.c'],
    dependencies: gtk_dep, include_directories: include_directories('.'))
  test('Engine favicon cache', favicon_test)
  origin_test = executable('engine-origin-test', ['tests/origin-test.cpp', 'ephy-engine-origin.cpp'],
    dependencies: [glib_dep, cef_dep, wrapper_dep], include_directories: engine_includes,
    cpp_args: ['-DCEF_RESOURCE_DIR="' + cef_root / 'Resources' + '"'])
  test('Engine Chromium origins', find_program('python3'),
    args: [files('tools/check-native.py'), origin_test], timeout: 60)
endif
