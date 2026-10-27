#!/usr/bin/env python3
"""Inside check-native, verify Chromium cookie enumeration and filtered clearing in Website Data."""
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
root = Path(tempfile.mkdtemp(prefix='gnosis-cookies-ui-', dir='/tmp'))
env = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), GSETTINGS_BACKEND='keyfile',
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()))
for key, value in [('ask-for-default', 'false'), ('active-clear-data-items', '1')]:
    subprocess.run(['gsettings', 'set', 'com.neg-zero.Gnosis', key, value], env=env, check=True)
(root/'profile').mkdir()
base = os.environ['GNOSIS_TEST_URL']
log = (root/'app.log').open('w')
command = [str(args.binary.resolve()), '--profile='+str(root/'profile')]
if args.incognito:
    command.append('--incognito-mode')
process = subprocess.Popen(command+[base+'cookies-ui-fixture'], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

def reports():
    if process.poll() is not None:
        raise RuntimeError((root/'app.log').read_text())
    with urllib.request.urlopen(base+'settings-reports', timeout=2) as response:
        return {r['domain']: r for r in json.load(response) if 'httpCookies' in r}

def wait_for(predicate, retry=None):
    deadline = time.monotonic()+20
    while time.monotonic() < deadline:
        if predicate():
            return
        if retry:
            retry()
        time.sleep(.5 if retry else .1)
    raise TimeoutError('Cookie UI did not produce the expected result: '+str(reports())+'\n'+(root/'app.log').read_text())

def key(*keys):
    subprocess.run(['xdotool', 'key', *keys], check=True)

def close_preferences():
    for _ in range(3):
        key('Escape')
        time.sleep(.3)

try:
    wait_for(lambda: 'httpCookie=127.0.0.1' in reports().get('127.0.0.1', {}).get('httpCookies', ''))
    window = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)], text=True).splitlines()[-1]
    subprocess.run(['xdotool', 'windowfocus', '--sync', window], check=True)
    key('ctrl+t', 'ctrl+l')
    subprocess.run(['xdotool', 'type', '--clearmodifiers', base.replace('127.0.0.1', 'localhost')+'cookies-ui-fixture'], check=True)
    key('Return')
    wait_for(lambda: 'httpCookie=localhost' in reports().get('localhost', {}).get('httpCookies', ''))
    assert 'httpCookie=' not in reports()['127.0.0.1']['jsCookies'], 'Fixture cookie was not HttpOnly'
    key('ctrl+shift+Delete')
    time.sleep(.7)
    subprocess.run(['xdotool', 'mousemove', '242', '108', 'click', '1'], check=True)
    time.sleep(.3)
    subprocess.run(['xdotool', 'mousemove', '400', '147', 'click', '1'], check=True)
    subprocess.run(['xdotool', 'type', '--delay', '20', '127.0.0.1'], check=True)
    time.sleep(.5)
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'filtered-cookies.png')], check=True)
    key('alt+c')
    wait_for(lambda: reports().get('127.0.0.1', {}).get('httpCookies') == '')
    assert 'httpCookie=localhost' in reports()['localhost']['httpCookies'], 'Filtered clearing removed another domain'
    assert 'pageCookie=localhost' in reports()['localhost']['jsCookies']
    assert all(r['storage'] == 'preserved' for r in reports().values()), 'Cookie clearing removed localStorage'
    print('Filtered Website Data clearing removes actual Chromium cookies, including HttpOnly, and preserves other domains/storage: PASS', flush=True)
    subprocess.run(['xdotool', 'mousemove', '400', '147', 'click', '1'], check=True)
    key('ctrl+a', 'BackSpace')
    time.sleep(.5)
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'before-all-cookies.png')], check=True)
    key('alt+c')
    time.sleep(.2)
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'clearing-all-cookies.png')], check=True)
    # Network reports can precede the store flush; the button stays disabled until it finishes.
    wait_for(lambda: reports().get('localhost', {}).get('httpCookies') == '', lambda: key('alt+c'))
    assert all(r['jsCookies'] == '' for r in reports().values()), reports()
    assert all(r['storage'] == 'preserved' for r in reports().values()), reports()
    print('Website Data clears all remaining Chromium cookies'+(' in incognito mode' if args.incognito else '')+': PASS', flush=True)
    close_preferences()
    key('ctrl+q')
    assert process.wait(timeout=10) == 0, (root/'app.log').read_text()
    print('Cookie dialog closes and the browser quits normally: PASS', flush=True)
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
    print('Cookie UI artifacts:', root, flush=True)
