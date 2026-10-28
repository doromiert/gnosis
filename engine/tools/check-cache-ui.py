#!/usr/bin/env python3
"""Inside check-native, verify the actual Website Data dialog clears Chromium's HTTP cache."""
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
root = Path(tempfile.mkdtemp(prefix='gnosis-cache-ui-', dir='/tmp'))
env = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), GSETTINGS_BACKEND='keyfile',
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()))
for key, value in [('ask-for-default', 'false'), ('active-clear-data-items', '2')]:
    subprocess.run(['gsettings', 'set', 'com.neg-zero.Gnosis', key, value], env=env, check=True)
(root/'profile').mkdir()
base = os.environ['GNOSIS_TEST_URL']
log = (root/'app.log').open('w')
command = [str(args.binary.resolve()), '--profile='+str(root/'profile')]
if args.incognito:
    command.append('--incognito-mode')
process = subprocess.Popen(command+[base+'cache-ui-fixture'], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

def reports():
    if process.poll() is not None:
        raise RuntimeError((root/'app.log').read_text())
    with urllib.request.urlopen(base+'settings-reports', timeout=2) as response:
        return [r['cache'] for r in json.load(response) if 'cache' in r]

def wait_for(predicate):
    deadline = time.monotonic()+20
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.1)
    raise TimeoutError('The actual cache dialog did not produce the expected result: '+str(reports())+'\n'+(root/'app.log').read_text())

try:
    wait_for(lambda: len(reports()) >= 3)
    original = reports()[-1]
    assert reports()[-3:] == [original]*3, reports()
    window = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)], text=True).splitlines()[-1]
    subprocess.run(['xdotool', 'windowfocus', '--sync', window, 'key', 'ctrl+shift+Delete'], check=True)
    time.sleep(.7)
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'cache-dialog.png')], check=True)
    subprocess.run(['xdotool', 'mousemove', '257', '174', 'click', '1'], check=True)
    wait_for(lambda: subprocess.check_output(['gsettings', 'get', 'com.neg-zero.Gnosis', 'active-clear-data-items'], env=env, text=True).strip() == '0')
    subprocess.run(['xdotool', 'key', 'alt+c'], check=True)
    time.sleep(.7)
    assert reports()[-1] == original, 'An unchecked cache row cleared data'
    subprocess.run(['xdotool', 'mousemove', '257', '174', 'click', '1'], check=True)
    wait_for(lambda: subprocess.check_output(['gsettings', 'get', 'com.neg-zero.Gnosis', 'active-clear-data-items'], env=env, text=True).strip() == '2')
    subprocess.run(['xdotool', 'mousemove', '242', '108', 'click', '1'], check=True)
    time.sleep(.3)
    subprocess.run(['xdotool', 'mousemove', '400', '147', 'click', '1'], check=True)
    subprocess.run(['xdotool', 'type', '--delay', '20', 'no-matching-website'], check=True)
    time.sleep(.5)
    subprocess.run(['xdotool', 'key', 'alt+c'], check=True)
    time.sleep(.7)
    assert reports()[-1] == original, 'A filtered-out cache row cleared data'
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'filtered-cache.png')], check=True)
    subprocess.run(['xdotool', 'mousemove', '400', '147', 'click', '1'], check=True)
    subprocess.run(['xdotool', 'key', 'ctrl+a', 'BackSpace'], check=True)
    time.sleep(.5)
    subprocess.run(['xdotool', 'key', 'alt+c'], check=True)
    wait_for(lambda: reports()[-1] != original)
    print('Actual Website Data dialog clears Chromium HTTP cache'+(' in incognito mode' if args.incognito else '')+': PASS', flush=True)
    for _ in range(3):
        subprocess.run(['xdotool', 'key', 'Escape'], check=True)
        time.sleep(.3)
    subprocess.run(['xdotool', 'key', 'ctrl+q'], check=True)
    assert process.wait(timeout=10) == 0, (root/'app.log').read_text()
    print('Website Data dialog closes and the browser quits normally: PASS', flush=True)
except BaseException:
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'failure.png')])
    with (root/'windows.log').open('w') as windows:
        subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', '.*', 'getwindowname', '%@'], stdout=windows, stderr=subprocess.STDOUT)
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
    print('Cache UI artifacts:', root, flush=True)
