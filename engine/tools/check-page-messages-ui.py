#!/usr/bin/env python3
"""Exercise internal page messages through Epiphany's real Reload and web-app UI."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--schemas', type=Path, required=True)
parser.add_argument('--incognito', action='store_true')
args = parser.parse_args()
root = Path(tempfile.mkdtemp(prefix='gnosis-page-messages-ui-', dir='/tmp'))
env = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), GSETTINGS_BACKEND='keyfile',
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()))
subprocess.run(['gsettings', 'set', 'com.neg-zero.Gnosis', 'ask-for-default', 'false'], env=env, check=True)
base = os.environ['GNOSIS_TEST_URL']
app_id = 'com.neg-zero.Gnosis.WebApp_native-message-fixture'
fixture = root/'data'/app_id
fixture.mkdir(parents=True)
(fixture/'.app').touch()
desktop = root/'data/applications'/f'{app_id}.desktop'
desktop.parent.mkdir()
icon = root/'fixture.png'
with urllib.request.urlopen(base+'favicon.png', timeout=2) as response:
    icon.write_bytes(response.read())
desktop.write_text('[Desktop Entry]\nType=Application\nName=Native message fixture\n'
                   f'Exec={args.binary.resolve()} {base}fixture\nIcon={icon}\n')
log = (root/'app.log').open('w')
command = [str(args.binary.resolve()), '--profile='+str(root/'profile')]
if args.incognito:
    command.append('--incognito-mode')
process = subprocess.Popen(command+[base+'page-retry-fixture'],
                           env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

def key(*keys):
    subprocess.run(['xdotool', 'key', '--clearmodifiers', '--delay', '100', *keys], check=True)

def click(x, y):
    subprocess.run(['xdotool', 'mousemove', str(x), str(y), 'click', '1'], check=True)

def reports():
    with urllib.request.urlopen(base+'settings-reports', timeout=2) as response:
        return json.load(response)

def wait_for(predicate):
    deadline = time.monotonic()+15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((root/'app.log').read_text())
        if predicate():
            return
        time.sleep(.1)
    raise TimeoutError('Page message UI did not finish: '+repr(reports())+'\n'+(root/'app.log').read_text())

def screenshot(name):
    subprocess.run(['magick', 'import', '-window', 'root', str(root/name)], check=True)

def pixels(name):
    return subprocess.check_output(['magick', str(root/name), '-alpha', 'off', '-depth', '8', 'rgb:-'])

def delete_button():
    width, height = map(int, subprocess.check_output(['magick', 'identify', '-format', '%w %h', str(root/'applications.png')], text=True).split())
    rgb = pixels('applications.png')
    points = [(i//3 % width, i//3//width) for i in range(0, len(rgb), 3)
              if i//3 % width > width//3 and i//3//width > 80 and rgb[i] > 160 and rgb[i+1] < 90 and rgb[i+2] < 100]
    assert len(points) > 300, 'The actual styled Delete button did not render'
    return ((min(x for x, y in points)+max(x for x, y in points))//2,
            (min(y for x, y in points)+max(y for x, y in points))//2)

try:
    wait_for(lambda: any(r.get('pageRetryAttempt') for r in reports()))
    window = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)], text=True).splitlines()[-1]
    subprocess.run(['xdotool', 'windowfocus', '--sync', window], check=True)
    time.sleep(3)
    screenshot('error.png')
    with urllib.request.urlopen(base+'page-retry-enable', timeout=2):
        pass
    click(500, 597)
    wait_for(lambda: any(r.get('pageRetryRecovered') for r in reports()))
    assert all(r['internalAPI'] == 'undefined' for r in reports() if r.get('pageRetryRecovered')), reports()
    screenshot('recovered.png')
    print('Chromium load failure renders Epiphany’s error page; its actual Reload action navigates to recovered content and drops the internal API: PASS', flush=True)
    key('ctrl+l')
    subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '1', 'about:applications'], check=True)
    key('Return')
    time.sleep(3)
    screenshot('applications.png')
    click(*delete_button())
    time.sleep(.6)
    screenshot('delete-dialog.png')
    before, modal = pixels('applications.png'), pixels('delete-dialog.png')
    assert sum(before[i:i+3] != modal[i:i+3] for i in range(0, len(before), 3)) > len(before)//12, 'The native confirmation dialog did not appear'
    key('Escape')
    time.sleep(.3)
    assert (fixture/'.app').exists() and desktop.exists()
    screenshot('cancelled.png')
    key('ctrl+q')
    assert process.wait(timeout=8) == 0, (root/'app.log').read_text()
    print('The actual web-app Delete control opens the native confirmation; cancellation preserves the disposable fixture and normal shutdown passes: PASS', flush=True)
except BaseException:
    screenshot('failure.png')
    raise
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    log.close()
    print('Page message UI artifacts:', root, flush=True)
