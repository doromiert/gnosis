#!/usr/bin/env python3
"""Check exact URL text through real GTK keyboard editing and page navigation."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--schemas', type=Path, required=True)
parser.add_argument('--clean-app-environment', action='store_true')
parser.add_argument('--expected-gtk', type=Path, help='Assert the actual browser loads this GTK library')
args = parser.parse_args()
base = os.environ['GNOSIS_TEST_URL']
artifacts = Path(tempfile.mkdtemp(prefix='gnosis-location-entry-'))
print('Location entry artifacts:', artifacts, flush=True)
env = dict(os.environ, GSETTINGS_BACKEND='keyfile', GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()),
           XDG_CONFIG_HOME=str(artifacts/'config'), XDG_CACHE_HOME=str(artifacts/'cache'),
           XDG_DATA_HOME=str(artifacts/'data'), LC_ALL='C.UTF-8')
app_env = dict(env)
if args.clean_app_environment:
    for name in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'CEF_ROOT', 'UBLOCK_DIR'):
        app_env.pop(name, None)
browser = None
results = []
gtk = ctypes.CDLL(str(Path(subprocess.check_output(['pkg-config', '--variable=libdir', 'gtk4'], text=True).strip())/'libgtk-4.so.1'))
glib = ctypes.CDLL(str(Path(subprocess.check_output(['pkg-config', '--variable=libdir', 'glib-2.0'], text=True).strip())/'libglib-2.0.so.0'))
gtk.gdk_display_get_default.restype = ctypes.c_void_p
gtk.gdk_display_get_clipboard.argtypes = [ctypes.c_void_p]
gtk.gdk_display_get_clipboard.restype = ctypes.c_void_p
gtk.gdk_clipboard_set_text.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
callback_type = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
gtk.gdk_clipboard_read_text_async.argtypes = [ctypes.c_void_p, ctypes.c_void_p, callback_type, ctypes.c_void_p]
gtk.gdk_clipboard_read_text_finish.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
gtk.gdk_clipboard_read_text_finish.restype = ctypes.c_void_p
glib.g_main_context_iteration.argtypes = [ctypes.c_void_p, ctypes.c_int]
glib.g_free.argtypes = [ctypes.c_void_p]
glib.g_error_free.argtypes = [ctypes.c_void_p]


def run(*command):
    return subprocess.check_output(list(map(str, command)), env=env, text=True, timeout=10).strip()


def key(*keys):
    run('xdotool', 'key', '--clearmodifiers', *keys)


def wait(predicate, description):
    deadline = time.monotonic()+20
    while time.monotonic() < deadline:
        if browser and browser.poll() is not None:
            raise RuntimeError('Gnosis exited while waiting for '+description)
        if predicate():
            return
        time.sleep(.05)
    raise TimeoutError(description)


def reports():
    with urllib.request.urlopen(base+'settings-reports', timeout=3) as response:
        return json.load(response)


def report(case):
    return next((r for r in reversed(reports()) if r.get('openURI') == case and r.get('event') == 'ready'), None)


def address(case):
    return base+'open-uri-fixture?'+urllib.parse.urlencode(dict(case=case, target='blank', mode='inert'))


def copy_entry():
    copied = []
    pending = [False]
    clipboard = gtk.gdk_display_get_clipboard(gtk.gdk_display_get_default())
    sentinel = 'location-entry-pending-'+str(time.monotonic_ns())
    gtk.gdk_clipboard_set_text(clipboard, sentinel.encode())
    key('ctrl+a'); key('ctrl+c')
    @callback_type
    def done(source, result, _data):
        pending[0] = False
        error = ctypes.c_void_p()
        text = gtk.gdk_clipboard_read_text_finish(source, result, ctypes.byref(error))
        if text:
            value = ctypes.string_at(text).decode()
            if value != sentinel:
                copied.append(value)
            glib.g_free(text)
        if error.value:
            glib.g_error_free(error)
    deadline = time.monotonic()+3
    while not copied and time.monotonic() < deadline:
        if not pending[0]:
            pending[0] = True
            gtk.gdk_clipboard_read_text_async(clipboard, None, done, None)
        glib.g_main_context_iteration(None, False)
        time.sleep(.002)
    assert copied, 'Actual GTK clipboard read timed out'
    return copied[0]


try:
    gtk.gtk_init()
    run('gsettings', 'set', 'com.neg-zero.Gnosis', 'ask-for-default', 'false')
    run('gsettings', 'set', 'com.neg-zero.Gnosis', 'restore-session-policy', "'crashed'")
    with (artifacts/'browser.log').open('w') as log:
        browser = subprocess.Popen([str(args.binary.resolve()), '--profile='+str(artifacts/'profile'),
                                    address('initial-long-history-entry')], env=app_env, stdout=log, stderr=subprocess.STDOUT)
        wait(lambda: report('initial-long-history-entry'), 'initial real fixture')
        window = run('xdotool', 'search', '--onlyvisible', '--pid', browser.pid).splitlines()[-1]
        run('xdotool', 'windowfocus', '--sync', window)
        wait(lambda: 'Gnosis OpenURI initial-long-history-entry' in run('xdotool', 'getwindowname', window),
             'initial committed page title')
        gtk_libraries = sorted({line.split()[-1] for line in Path('/proc/'+str(browser.pid)+'/maps').read_text().splitlines()
                                if '/libgtk-4.so' in line})
        (artifacts/'runtime.json').write_text(json.dumps(dict(binary=str(args.binary.resolve()), gtk_libraries=gtk_libraries), indent=2)+'\n')
        assert len(gtk_libraries) == 1, 'Browser loaded multiple GTK libraries'
        if args.expected_gtk:
            assert gtk_libraries == [str(args.expected_gtk.resolve())], gtk_libraries
        for case, delay, clear in [('short', 5, True), ('different-query-0123456789', 1, True),
                                   ('third', 5, False), ('fourth-longer-query', 1, False)]:
            expected = address(case)
            key('ctrl+l')
            if clear:
                key('ctrl+a'); key('BackSpace')
            run('xdotool', 'type', '--clearmodifiers', '--delay', delay, expected)
            time.sleep(.1)
            actual = copy_entry()
            results.append(dict(case=case, expected=expected, entry=actual, delay=delay, explicit_clear=clear))
            print(json.dumps(results[-1]), flush=True)
            assert actual == expected, 'URL entry retained or inserted unexpected text'
            key('Return')
            wait(lambda: report(case), 'actual fixture '+case)
            results[-1]['pageURL'] = report(case)['pageURL']
            assert results[-1]['pageURL'] == expected, 'Navigation changed the typed URL'
        # A unique visited query exercises actual inline completion, including
        # the display subtitle's escaped '&' characters, rather than disabling it.
        case = 'different-query-0123456789'
        expected = address(case)
        prefix = base+'open-uri-fixture?case=different-query-01234'
        key('ctrl+l'); key('ctrl+a'); key('BackSpace')
        run('xdotool', 'type', '--clearmodifiers', '--delay', 5, prefix)
        time.sleep(.3)
        run('xdotool', 'type', '--clearmodifiers', '5')
        time.sleep(.2)
        actual = copy_entry()
        results.append(dict(case='inline-completion', expected=expected, entry=actual))
        print(json.dumps(results[-1]), flush=True)
        assert actual == expected, 'Inline completion inserted markup or failed to complete the visited URL'
        previous = sum(r.get('openURI') == case and r.get('event') == 'ready' for r in reports())
        key('Return')
        wait(lambda: sum(r.get('openURI') == case and r.get('event') == 'ready' for r in reports()) > previous,
             'actual completed URL navigation')
        results[-1]['pageURL'] = report(case)['pageURL']
        assert results[-1]['pageURL'] == expected, 'Completed URL changed in navigation'
        key('ctrl+q')
        assert browser.wait(timeout=15) == 0, 'Gnosis did not quit normally'
    print('LOCATION_ENTRY:PASS (exact GTK text/committed URLs, changing history/query lengths, raw-URL inline completion)', flush=True)
finally:
    (artifacts/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    (artifacts/'server-observations.json').write_text(json.dumps(reports(), indent=2)+'\n')
    run('magick', 'import', '-window', 'root', artifacts/'final.png')
    if browser and browser.poll() is None:
        browser.terminate()
        try:
            browser.wait(timeout=5)
        except subprocess.TimeoutExpired:
            browser.kill(); browser.wait()
