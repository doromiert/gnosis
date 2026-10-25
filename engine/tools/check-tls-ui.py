#!/usr/bin/env python3
"""Inside check-native, exercise Epiphany's real CEF-backed TLS error UI."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
from urllib.parse import urlsplit, urlunsplit

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--schemas', type=Path, required=True)
parser.add_argument('--incognito', action='store_true')
args = parser.parse_args()
root = Path(tempfile.mkdtemp(prefix='jupiter-tls-ui-', dir='/tmp'))
env = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), GSETTINGS_BACKEND='keyfile',
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()))
subprocess.run(['gsettings', 'set', 'io.github.doromiert.Jupiter', 'ask-for-default', 'false'],
               env=env, check=True)
fixtures = json.loads(os.environ['JUPITER_TLS_FIXTURES'])
required = {'plain', 'valid', 'replacement'}
assert required <= fixtures.keys(), 'Missing TLS UI fixtures: '+repr(required-fixtures.keys())
log = (root/'app.log').open('w')
command = [str(args.binary.resolve()), '--profile='+str(root/'profile')]
if args.incognito:
    command.append('--incognito-mode')
process = subprocess.Popen(command+[fixtures['valid']], env=env, stdout=log,
                           stderr=subprocess.STDOUT, start_new_session=True)
window = None

def key(*keys):
    subprocess.run(['xdotool', 'key', '--clearmodifiers', '--delay', '100', *keys], check=True)

def title():
    return subprocess.check_output(['xdotool', 'getwindowname', window], text=True).strip()

def wait_for_title(expected):
    deadline = time.monotonic()+20
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((root/'app.log').read_text())
        if expected in title():
            return
        time.sleep(.1)
    raise TimeoutError('Expected window title '+repr(expected)+', got '+repr(title())+'\n'+
                       (root/'app.log').read_text())

def screenshot(name):
    path = root/name
    subprocess.run(['magick', 'import', '-window', 'root', str(path)], check=True)
    return path

def pixels(path):
    return subprocess.check_output(['magick', str(path), '-alpha', 'off', '-depth', '8', 'rgb:-'])

def red_pixels(path):
    rgb = pixels(path)
    return sum(rgb[i] > 160 and rgb[i+1] < 135 and rgb[i+2] < 135
               for i in range(0, len(rgb), 3))

def difference(first, second):
    before, after = pixels(first), pixels(second)
    assert len(before) == len(after)
    return sum(before[i:i+3] != after[i:i+3] for i in range(0, len(before), 3))

def geometry():
    output = subprocess.check_output(['xdotool', 'getwindowgeometry', '--shell', window], text=True)
    values = dict(line.split('=', 1) for line in output.splitlines() if '=' in line)
    return tuple(int(values[key]) for key in ('X', 'Y', 'WIDTH', 'HEIGHT'))

def navigate(url):
    key('ctrl+l')
    subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '1', url], check=True)
    key('Return')

def settle_page():
    key('Escape')
    time.sleep(.5)

def other_host(url):
    parts = urlsplit(url)
    port = ':'+str(parts.port) if parts.port else ''
    return urlunsplit((parts.scheme, 'localhost'+port, parts.path, parts.query, parts.fragment))

def window_metadata():
    lines = []
    result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', '.*'],
                            capture_output=True, text=True)
    for identifier in result.stdout.splitlines():
        name = subprocess.run(['xdotool', 'getwindowname', identifier], capture_output=True, text=True)
        size = subprocess.run(['xdotool', 'getwindowgeometry', '--shell', identifier],
                              capture_output=True, text=True)
        lines.append(identifier+' '+name.stdout.strip()+'\n'+size.stdout)
    (root/'windows.log').write_text('\n'.join(lines))

try:
    deadline = time.monotonic()+20
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((root/'app.log').read_text())
        found = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)],
                               capture_output=True, text=True)
        if found.stdout.splitlines():
            window = found.stdout.splitlines()[-1]
            break
        time.sleep(.1)
    assert window, 'Jupiter did not create a visible browser window'
    subprocess.run(['xdotool', 'windowfocus', '--sync', window], check=True)
    wait_for_title('Security Violation')
    time.sleep(.5)
    closed = screenshot('valid-error.png')
    assert red_pixels(closed) > 100, 'Epiphany danger styling did not render on the TLS error page'

    x, y, width, height = geometry()
    subprocess.run(['xdotool', 'mousemove', str(x+width//2), str(y+height//2), 'click', '1'], check=True)
    key('Tab', 'Return')
    time.sleep(.5)
    expanded = screenshot('valid-details.png')
    assert difference(closed, expanded) > 1000, 'Technical information did not visibly expand'
    assert red_pixels(expanded) > red_pixels(closed)+500, 'Accept Risk action did not become visible'

    key('Tab', 'Return')
    wait_for_title('TLS fixture valid')
    settle_page()
    screenshot('valid-accepted.png')
    print('Epiphany\'s real TLS page renders danger styling; Technical information reveals the actual Accept Risk action and native acceptance reloads the intended page: PASS', flush=True)

    navigate(fixtures['replacement'])
    wait_for_title('Security Violation')
    settle_page()
    screenshot('replacement-rejected.png')
    navigate(fixtures['plain'])
    wait_for_title('TLS fixture plain')
    navigate(other_host(fixtures['valid']))
    wait_for_title('Security Violation')
    settle_page()
    screenshot('other-host-rejected.png')
    navigate(fixtures['valid'])
    wait_for_title('TLS fixture valid')
    settle_page()
    screenshot('exact-exception-reloaded.png')
    print('The TLS exception is scoped to the accepted certificate and host; replacement certificates and another hostname still show Epiphany\'s error page: PASS', flush=True)

    key('ctrl+q')
    assert process.wait(timeout=10) == 0, (root/'app.log').read_text()
    print(('Private' if args.incognito else 'Regular')+' TLS UI workflow quits normally: PASS', flush=True)
except BaseException:
    if window:
        subprocess.run(['magick', 'import', '-window', 'root', str(root/'failure.png')], check=False)
    window_metadata()
    raise
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    log.close()
    print('TLS UI artifacts:', root, flush=True)
