fs = import('fs')
cef_root = get_option('cef_root')
ublock_dir = get_option('ublock_dir')
if cef_root == '' or ublock_dir == ''
  error('Set cef_root to the pinned CEF SDK and ublock_dir to official full uBlock Origin; nix develop supplies both.')
endif
# ICU is located relative to the loaded libcef.so, including development builds.
foreach name : ['libcef.so', 'libEGL.so', 'libGLESv2.so', 'libvulkan.so.1', 'libvk_swiftshader.so', 'vk_swiftshader_icd.json', 'snapshot_blob.bin', 'v8_context_snapshot.bin', 'chrome-sandbox']
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
  ['ephy-engine-download.cpp', 'ephy-engine-permission.cpp', 'ephy-engine-session.cpp', 'ephy-engine-response.cpp', 'ephy-engine-settings.cpp', 'ephy-engine-origin.cpp', 'ephy-engine-favicon.c', 'ephy-engine-view.cpp', 'ephy-engine-scheme.cpp', 'ephy-engine-print.cpp', 'select/application.cpp', 'select/browser.cpp', 'select/file-dialog.cpp', 'select/page-messages.cpp', 'select/clipboard.cpp',
   'select/cursor.cpp', 'select/policy.cpp', 'select/extensions.cpp', 'select/extension-host.cpp', engine_resource],
  include_directories: engine_includes,
  dependencies: [gtk_dep, libadwaita_dep, json_glib_dep, libsoup_dep, webkitgtk_dep, dependency('epoxy'), cef_dep, wrapper_dep],
  cpp_args: ['-DCEF_RESOURCE_DIR="' + cef_root / 'Resources' + '"',
    '-DJUPITER_UBLOCK_DIR="' + ublock_dir + '"',
    '-DJUPITER_BRIDGE_DIR="' + bridge_dir + '"'],
  pic: true)
ephyengine_dep = declare_dependency(link_with: engine, include_directories: include_directories('.'), dependencies: [cef_dep, wrapper_dep, json_glib_dep])
install_subdir(cef_root / 'Resources', install_dir: pkglibdir / 'cef', strip_directory: true)
install_subdir(cef_root / 'Release', install_dir: pkglibdir / 'cef', strip_directory: true)
install_subdir('data/tab-bridge', install_dir: pkgdatadir)
install_data(cef_root / 'LICENSE.txt', cef_root / 'CREDITS.html', install_dir: datadir / 'licenses/jupiter-cef')
if get_option('unit_tests').enabled()
  clipboard_test = executable('engine-clipboard-test', 'tests/capabilities-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium clipboard', find_program('python3'),
    args: [files('tools/check-native.py'), clipboard_test, '--clipboard-only'], timeout: 90, is_parallel: false)
  page_messages_test = executable('engine-page-messages-test', 'tests/page-messages-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine internal page messages', find_program('python3'),
    args: [files('tools/check-native.py'), page_messages_test], timeout: 90, is_parallel: false)
  # Parallel CEF/GTK processes currently expose unresolved runtime timeouts.
  native_permission_test = executable('engine-permission-test', 'tests/permission-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium permissions', find_program('python3'),
    args: [files('tools/check-native.py'), native_permission_test, '--use-fake-device-for-media-stream'], timeout: 60, is_parallel: false)
  script_test = executable('engine-script-test', 'tests/script-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium scripts', find_program('python3'),
    args: [files('tools/check-native.py'), script_test], timeout: 60, is_parallel: false)
  session_test = executable('engine-session-test', 'tests/session-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep, libsoup_dep],
    include_directories: engine_includes)
  test('Engine Chromium sessions', find_program('python3'),
    args: [files('tools/check-native.py'), session_test], timeout: 60, is_parallel: false)
  settings_test = executable('engine-settings-test', 'tests/settings-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium settings', find_program('python3'),
    args: [files('tools/check-native.py'), settings_test], timeout: 60, is_parallel: false)
  test('Engine Chromium feature overrides', find_program('python3'),
    args: [files('tools/check-native.py'), settings_test, '--flags'], timeout: 60, is_parallel: false)
  response_test = executable('engine-response-test', 'tests/response-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium responses', find_program('python3'),
    args: [files('tools/check-native.py'), response_test], timeout: 60, is_parallel: false)
  download_test = executable('engine-download-test', 'tests/download-test.cpp',
    dependencies: [ephyengine_dep, gtk_dep, libadwaita_dep, webkitgtk_dep],
    include_directories: engine_includes)
  test('Engine Chromium downloads', find_program('python3'),
    args: [files('tools/check-native.py'), download_test], timeout: 60, is_parallel: false)
  diagnostic_test = executable('diagnostic-process-test', 'tests/diagnostic-process-test.cpp')
  test('Diagnostic process failures', diagnostic_test)
  permission_test = executable('extension-permissions-test', 'tests/extension-permissions-test.cpp',
    dependencies: glib_dep, include_directories: include_directories('select'))
  test('Extension host permissions', permission_test)
  favicon_test = executable('engine-favicon-test', ['tests/favicon-test.c', 'ephy-engine-favicon.c'],
    dependencies: gtk_dep, include_directories: include_directories('.'))
  test('Engine favicon cache', favicon_test)
  origin_test = executable('engine-origin-test', ['tests/origin-test.cpp', 'ephy-engine-origin.cpp'],
    dependencies: [glib_dep, cef_dep, wrapper_dep], include_directories: engine_includes,
    cpp_args: ['-DCEF_RESOURCE_DIR="' + cef_root / 'Resources' + '"'])
  test('Engine Chromium origins', find_program('python3'),
    args: [files('tools/check-native.py'), origin_test], timeout: 60, is_parallel: false)
endif
