#!/usr/bin/env python3
"""Inside check-native --open-uri, test the real app, dialog, portal and handler."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--schemas', type=Path, default=os.environ.get('GSETTINGS_SCHEMA_DIR'))
parser.add_argument('--inspect-prompt', action='store_true', help='Capture the first real prompt and wait up to 60 seconds for SIGUSR1')
parser.add_argument('--switch-offset', type=int, nargs=2, metavar=('X', 'Y'), help='Always Allow switch offset from the owning window center')
parser.add_argument('--open-offset', type=int, nargs=2, metavar=('X', 'Y'), help='Open Link button offset from the owning window center')
args = parser.parse_args()
if not args.schemas:
    parser.error('Provide --schemas or GSETTINGS_SCHEMA_DIR for the actual app')
if not args.inspect_prompt and (not args.switch_offset or not args.open_offset):
    parser.error('Inspect the prompt first, then provide --switch-offset and --open-offset')
scratch = Path(os.environ['GNOSIS_TEST_SCRATCH'])
artifacts = Path(tempfile.mkdtemp(prefix='gnosis-open-uri-ui-', dir='/tmp'))
print('OpenURI artifacts:', artifacts, flush=True)
env = dict(os.environ, GSETTINGS_BACKEND='keyfile', GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()), LC_ALL='C.UTF-8')
base = os.environ['GNOSIS_TEST_URL']
web_schema = 'com.neg-zero.Gnosis.web:/com/neg_zero/gnosis/web/'
processes, logs = [], []
browser = None
window = None

def run(*command, **kwargs):
    return subprocess.check_output(list(map(str, command)), env=env, text=True, timeout=10, **kwargs).strip()

def xdo(*command):
    return run('xdotool', *command)

def key(*keys):
    xdo('key', '--clearmodifiers', *keys)

def wait(predicate, description, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if browser and browser.poll() is not None:
            raise RuntimeError('Browser exited while waiting for ' + description)
        if predicate():
            return
        time.sleep(.1)
    raise TimeoutError(description)

def screenshot(name):
    run('magick', 'import', '-window', 'root', artifacts / (name + '.png'))

def geometry(w):
    return {k: int(v) for k, v in (line.split('=', 1) for line in xdo('getwindowgeometry', '--shell', w).splitlines()) if k in ('X', 'Y', 'WIDTH', 'HEIGHT')}

def windows():
    result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(browser.pid)], capture_output=True, text=True)
    return {w for w in result.stdout.splitlines() if geometry(w)['WIDTH'] > 400 and geometry(w)['HEIGHT'] > 300}

def click(w, offset=(0, 0), focus=True):
    if focus:
        xdo('windowfocus', '--sync', w)
    size = geometry(w)
    xdo('mousemove', '--window', w, size['WIDTH']//2 + offset[0], size['HEIGHT']//2 + offset[1], 'click', 1)

def reports():
    with urllib.request.urlopen(base + 'settings-reports', timeout=2) as response:
        return json.load(response)

def report(case, event='ready'):
    return next((r for r in reversed(reports()) if r.get('openURI') == case and r.get('event') == event), None)

def address(case, target='blank', mode='link', cross=False):
    url = base.replace('127.0.0.1', 'localhost') if cross else base
    return url + 'open-uri-fixture?' + urllib.parse.urlencode(dict(case=case, target=target, mode=mode))

def navigate(case, **options):
    xdo('windowfocus', '--sync', window)
    key('ctrl+l'); xdo('type', '--clearmodifiers', '--delay', 1, address(case, **options)); key('Return')
    wait(lambda: report(case), 'real HTTP fixture ' + case)
    assert report(case)['pageURL'] == address(case, **options), report(case)
    time.sleep(.4)

def calls():
    path = artifacts / 'handler.jsonl'
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

def portal_calls():
    traffic = (artifacts / 'portal-bus.log').read_text()
    found = []
    for block in re.split(r'(?=method call |signal time=)', traffic):
        if 'interface=org.freedesktop.portal.OpenURI;' in block and 'member=OpenURI' in block:
            found.append(re.findall(r'^\s+string "(.*)"$', block, re.M)[:2])
    return found

def portal_responses():
    traffic = (artifacts / 'portal-bus.log').read_text()
    expected, responses = [], {}
    for block in re.split(r'(?=method call |signal time=)', traffic):
        if 'interface=org.freedesktop.portal.OpenURI;' in block and 'member=OpenURI' in block:
            sender = re.search(r' sender=([^ ]+)', block).group(1).lstrip(':').replace('.', '_')
            token = re.search(r'string "handle_token"\s+variant\s+string "([^"]+)"', block).group(1)
            expected.append('/org/freedesktop/portal/desktop/request/' + sender + '/' + token)
        elif 'interface=org.freedesktop.portal.Request;' in block and 'member=Response' in block:
            path = re.search(r' path=([^;]+);', block).group(1)
            responses.setdefault(path, []).append(int(re.search(r'\n\s+uint32 ([0-9]+)', block).group(1)))
    return [responses.get(path, []) for path in expected]

def grants():
    return run('gsettings', 'get', web_schema, 'auto-open-schemes')

def unchanged(count):
    time.sleep(.9)
    assert len(calls()) == count and len(portal_calls()) == count, (calls(), portal_calls())

def launched(case, w, count):
    expected = report(case)['expected']
    wait(lambda: len(calls()) == count, 'disposable handler invocation')
    wait(lambda: len(portal_calls()) == count, 'real OpenURI D-Bus call')
    assert calls()[-1] == [expected], calls()[-1]
    parent, uri = portal_calls()[-1]
    assert uri == expected and parent.startswith('x11:') and int(parent[4:], 16) == int(w), (parent, uri, w)
    wait(lambda: portal_responses() == [[0]] * count,
         'successful actual portal response')

def prompt(case, w=None, focus=True):
    click(w or window, focus=focus)
    wait(lambda: report(case, 'clicked'), 'trusted CEF click ' + case)
    time.sleep(.4)
    screenshot(case)

def launch(private=False):
    global browser, window
    log = (artifacts / ('private.log' if private else 'app.log')).open('a'); logs.append(log)
    command = [str(args.binary.resolve()), '--profile=' + str(scratch / ('private-profile' if private else 'profile'))]
    if private:
        command.append('--incognito-mode')
    browser = subprocess.Popen(command + [address('private-start' if private else 'start', mode='inert')], env=env, stdout=log, stderr=subprocess.STDOUT)
    processes.append(browser)
    wait(lambda: windows(), 'actual Gnosis window')
    window = sorted(windows())[-1]
    xdo('windowfocus', '--sync', window)
    wait(lambda: report('private-start' if private else 'start'), 'initial actual page')

def quit_browser():
    xdo('windowfocus', '--sync', window); key('ctrl+q')
    assert browser.wait(timeout=10) == 0, 'Gnosis failed normal quit'

def desktop_close(w):
    # A real window manager sends WM_DELETE_WINDOW; XDestroyWindow is not equivalent.
    class ClientMessage(ctypes.Structure):
        _fields_ = [('type', ctypes.c_int), ('serial', ctypes.c_ulong), ('send_event', ctypes.c_int),
                    ('display', ctypes.c_void_p), ('window', ctypes.c_ulong), ('message_type', ctypes.c_ulong),
                    ('format', ctypes.c_int), ('data', ctypes.c_long * 5)]
    class Event(ctypes.Union):
        _fields_ = [('client', ClientMessage), ('padding', ctypes.c_long * 24)]
    x11 = ctypes.CDLL(str(Path(run('pkg-config', '--variable=libdir', 'x11')) / 'libX11.so.6'))
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.POINTER(Event)]
    x11.XFlush.argtypes = x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    assert display, 'Could not open isolated X11 display'
    try:
        event = Event(); event.client.type = 33; event.client.send_event = 1
        event.client.display = display; event.client.window = int(w); event.client.format = 32
        event.client.message_type = x11.XInternAtom(display, b'WM_PROTOCOLS', 0)
        event.client.data[0] = x11.XInternAtom(display, b'WM_DELETE_WINDOW', 0)
        assert x11.XSendEvent(display, int(w), 0, 0, ctypes.byref(event))
        x11.XFlush(display)
    finally:
        x11.XCloseDisplay(display)

try:
    config = Path(env['XDG_CONFIG_HOME']) / 'xdg-desktop-portal'; config.mkdir(parents=True, exist_ok=True)
    (config / 'portals.conf').write_text('[preferred]\ndefault=gtk\norg.freedesktop.impl.portal.AppChooser=gtk\n')
    applications = Path(env['XDG_DATA_HOME']) / 'applications'; applications.mkdir(parents=True, exist_ok=True)
    handler = artifacts / 'record-handler.py'
    handler.write_text('#!/run/current-system/sw/bin/python3\nimport json,sys\nwith open(' + repr(str(artifacts/'handler.jsonl')) + ", 'a') as log: log.write(json.dumps(sys.argv[1:],ensure_ascii=False)+'\\n')\n")
    handler.chmod(0o755)
    (applications / 'gnosis-uri-test.desktop').write_text('[Desktop Entry]\nType=Application\nName=Gnosis Disposable URI Test\nExec=' + str(handler) + ' %u\nTerminal=false\nMimeType=x-scheme-handler/gnosis-test;\n')
    run('gio', 'mime', 'x-scheme-handler/gnosis-test', 'gnosis-uri-test.desktop')
    assert 'gnosis-uri-test.desktop' in run('gio', 'mime', 'x-scheme-handler/gnosis-test')
    for name in ('org.freedesktop.impl.portal.desktop.gtk', 'org.freedesktop.portal.Desktop'):
        entry = Path('/run/current-system/sw/share/dbus-1/services') / (name + '.service')
        command = next(line[5:] for line in entry.read_text().splitlines() if line.startswith('Exec='))
        log = (artifacts / (name + '.log')).open('w'); logs.append(log)
        processes.append(subprocess.Popen(shlex.split(command), env=env, stdout=log, stderr=subprocess.STDOUT))
        run('gdbus', 'wait', '--session', '--timeout=10', name)
    def portal_ready():
        return subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.portal.Desktop',
            '--object-path', '/org/freedesktop/portal/desktop', '--method', 'org.freedesktop.DBus.Properties.Get',
            'org.freedesktop.portal.OpenURI', 'version'], env=env, capture_output=True).returncode == 0
    wait(portal_ready, 'real OpenURI portal interface')
    monitor = (artifacts / 'portal-bus.log').open('w'); logs.append(monitor)
    processes.append(subprocess.Popen(['dbus-monitor', '--session', "interface='org.freedesktop.portal.OpenURI'", "interface='org.freedesktop.portal.Request'"], env=env, stdout=monitor, stderr=subprocess.STDOUT))
    run('gsettings', 'set', 'com.neg-zero.Gnosis', 'ask-for-default', 'false')
    run('gsettings', 'set', 'com.neg-zero.Gnosis', 'restore-session-policy', "'crashed'")
    assert grants() == '@a(ss) []' or grants() == '[]', grants()
    launch(); navigate('cancel'); prompt('cancel')
    if args.inspect_prompt:
        released = False
        def release(*_):
            global released
            released = True
        signal.signal(signal.SIGUSR1, release)
        print('OPEN_URI_INSPECT_READY window=' + window + ' geometry=' + json.dumps(geometry(window)) + ' pid=' + str(os.getpid()), flush=True)
        print('Screenshot:', artifacts/'cancel.png', flush=True)
        deadline = time.monotonic() + 60
        while not released and time.monotonic() < deadline:
            time.sleep(.2)
        key('Escape'); quit_browser()
    else:
        key('Escape'); unchanged(0)
        navigate('once'); prompt('once'); key('alt+o'); launched('once', window, 1)
        assert grants() in ('[]', '@a(ss) []')
        navigate('remember'); prompt('remember'); click(window, args.switch_offset); key('alt+o'); launched('remember', window, 2)
        wait(lambda: 'gnosis-test' in grants(), 'grant persisted only after portal success'); saved = grants()
        navigate('reuse'); click(window); launched('reuse', window, 3)
        navigate('direct', target='direct'); prompt('direct'); unchanged(3); key('alt+o'); launched('direct', window, 4)
        assert grants() == saved
        navigate('stale-nav', mode='navigate', cross=True); prompt('stale-nav')
        wait(lambda: report('stale-nav-after'), 'HTTP navigation cancels the old prompt'); key('alt+o'); unchanged(4)
        navigate('owner', cross=True)
        owner = window; before = windows(); key('ctrl+n'); wait(lambda: windows()-before, 'second actual window')
        other = next(iter(windows()-before)); window = other; navigate('other', mode='inert')
        xdo('windowraise', owner); xdo('windowfocus', '--sync', other)
        prompt('owner', owner, focus=False); xdo('windowfocus', '--sync', other)
        assert xdo('getwindowfocus') == other
        click(owner, args.open_offset, focus=False); launched('owner', owner, 5)
        assert xdo('getwindowfocus') == other, 'Owning-window test lost the independently focused second window'
        window = owner; quit_browser(); unchanged(5)
        launch(private=True); navigate('private-reuse'); prompt('private-reuse'); unchanged(5)
        key('alt+o'); launched('private-reuse', window, 6); assert grants() == saved
        navigate('private-new', cross=True); prompt('private-new'); key('alt+o'); launched('private-new', window, 7)
        assert grants() == saved
        navigate('stale-close', cross=True); prompt('stale-close'); desktop_close(window)
        try:
            assert browser.wait(timeout=5) == 0, 'Gnosis failed desktop close'
            close_proof = 'pending consent closed by actual WM_DELETE_WINDOW'
        except subprocess.TimeoutExpired:
            screenshot('desktop-close-pending')
            unchanged(7)
            key('Escape'); quit_browser()
            close_proof = 'modal GUI did not close the window; pending destruction covered by native lifecycle test only'
        unchanged(7)
        (artifacts / 'grants.txt').write_text(saved + '\n')
        assert portal_responses() == [[0]] * 7
        print('OPEN_URI_GUI_CLOSE:', close_proof, flush=True)
        print('OPEN_URI_UI:PASS (exact handler argv, seven actual parented portal calls, consent, grants, private isolation and stale navigation)', flush=True)
except BaseException:
    (artifacts / 'fixture-reports.json').write_text(json.dumps(reports(), indent=2))
    screenshot('failure')
    raise
finally:
    (artifacts / 'fixture-reports.json').write_text(json.dumps(reports(), indent=2))
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
    for log in logs:
        log.close()
    settings = Path(env['XDG_CONFIG_HOME']) / 'glib-2.0/settings/keyfile'
    if settings.exists():
        (artifacts / 'settings-keyfile').write_bytes(settings.read_bytes())
