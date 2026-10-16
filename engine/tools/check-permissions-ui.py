#!/usr/bin/env python3
"""Inside check-native, exercise Epiphany's real CEF-backed permission dialogs."""
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
args = parser.parse_args()
root = Path(tempfile.mkdtemp(prefix='jupiter-permissions-ui-', dir='/tmp'))
env = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), GSETTINGS_BACKEND='keyfile',
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()))
for setting, value in [('ask-for-default', 'false'), ('restore-session-policy', "'crashed'")]:
    subprocess.run(['gsettings', 'set', 'io.github.doromiert.Jupiter', setting, value], env=env, check=True)
base = os.environ['JUPITER_TEST_URL']
host = base.split('/')[2]
log = (root/'app.log').open('w')
process = None
test_id = None
launch_count = 0

def key(*keys):
    subprocess.run(['xdotool', 'key', *keys], check=True)

def report():
    with urllib.request.urlopen(base+'settings-reports', timeout=2) as response:
        matches = [r for r in json.load(response) if r.get('permissionPage') == host and r.get('testId') == test_id]
    return matches[-1] if matches else {}

def outcome():
    return report().get('outcome')

def states():
    return report().get('permissionStates', {})

def wait_for(predicate):
    deadline = time.monotonic()+15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((root/'app.log').read_text())
        if predicate():
            return
        time.sleep(.1)
    raise TimeoutError('Permission UI did not finish: '+str(outcome())+'\n'+(root/'app.log').read_text())

def launch(scope, private=False, permissions=None):
    global process, test_id, launch_count
    profile = root/scope
    profile.mkdir(exist_ok=True)
    if permissions is not None:
        (profile/'permissions.ini').write_text(permissions)
    launch_count += 1
    test_id = '?'+scope+str(launch_count)
    command = [str(args.binary.resolve()), '--profile='+str(profile)]
    if private:
        command.append('--incognito-mode')
    process = subprocess.Popen(command+[base+'permission-fixture'+test_id], env=env, stdout=log,
                               stderr=subprocess.STDOUT, start_new_session=True)
    wait_for(lambda: outcome() == 'idle' and len(states()) == 5)
    window = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)], text=True).splitlines()[-1]
    subprocess.run(['xdotool', 'windowfocus', '--sync', window], check=True)
    return profile

def permission_setting(index):
    subprocess.run(['xdotool', 'mousemove', '302', '30', 'click', '1'], check=True)
    key('s')
    time.sleep(.8)
    subprocess.run(['xdotool', 'mousemove', '560', '98', 'click', '1'], check=True)
    time.sleep(.5)
    subprocess.run(['xdotool', 'mousemove', '650', '226', 'click', '1'], check=True)
    time.sleep(.2)
    key('Home', *(['Down']*index), 'Return')
    time.sleep(.3)
    subprocess.run(['magick', 'import', '-window', 'root', str(root/('site-setting-'+str(index)+'.png'))], check=True)
    key('Escape')

def request(name, y=100, rich=False):
    command = ['xdotool', 'mousemove', '160', str(y)]
    if rich:
        command += ['keydown', 'Shift_L', 'click', '1', 'keyup', 'Shift_L']
    else:
        command += ['click', '1']
    subprocess.run(command, check=True)
    wait_for(lambda: outcome() == 'pending')
    time.sleep(.3)
    subprocess.run(['magick', 'import', '-window', 'root', str(root/(name+'.png'))], check=True)

def quit():
    key('ctrl+q')
    assert process.wait(timeout=10) == 0, (root/'app.log').read_text()

try:
    port = host.rsplit(':', 1)[1]
    saved = "[org/gnome/epiphany/permissions/http/127.0.0.1/"+port+"]\nnotifications-permission='deny'\n"
    profile = launch('regular', permissions=saved)
    assert report()['notificationState'] == 'denied' and states()['notifications'] == 'denied', report()
    print('Legacy notification denial is imported before the first page query: PASS', flush=True)
    permission_setting(2)
    wait_for(lambda: report()['notificationState'] == 'default' and states()['notifications'] == 'prompt')
    quit()
    saved = (profile/'permissions.ini').read_text()
    launch('regular')
    assert report()['notificationState'] == 'default', report()
    print('Native website controls revoke a saved denial; restart does not resurrect the legacy file choice: PASS', flush=True)
    request('ask-later')
    key('Return')
    wait_for(lambda: outcome() == 'default')
    assert (profile/'permissions.ini').read_text() == saved
    print('Epiphany Ask Later dismisses the actual Chromium request without saving a grant: PASS', flush=True)
    request('deny')
    key('alt+d')
    wait_for(lambda: outcome() == 'denied' and states()['notifications'] == 'denied')
    for attempt in range(2):
        request('clipboard-deny-'+str(attempt), 220, rich=bool(attempt))
        key('alt+d')
        wait_for(lambda: outcome() == 'NotAllowedError' and states()['clipboard-read'] == 'prompt')
    assert states()['microphone'] == 'prompt' and states()['camera'] == 'prompt', states()
    assert (profile/'permissions.ini').read_text() == saved
    print('Clipboard denial prompts again on the next request and leaves microphone/camera choices unchanged: PASS', flush=True)
    for attempt in range(2):
        request('clipboard-allow-'+str(attempt), 220, rich=bool(attempt))
        subprocess.run(['xdotool', 'mousemove', '500', '370', 'click', '1'], check=True)
        wait_for(lambda: outcome() == 'read' and states()['clipboard-read'] == 'prompt')
        assert report()['clipboardValue'] == 'GTK clipboard from native dialog', report()
    print('The actual GTK Allow/Deny dialogs gate text/rich clipboard reads, return exact text and prompt again on the next read: PASS', flush=True)
    quit()
    launch('regular')
    assert report()['notificationState'] == 'denied', report()
    permission_setting(0)
    wait_for(lambda: report()['notificationState'] == 'granted' and states()['notifications'] == 'granted')
    quit()
    launch('regular')
    assert report()['notificationState'] == 'granted', report()
    permission_setting(2)
    wait_for(lambda: states()['notifications'] == 'prompt')
    request('allow')
    subprocess.run(['xdotool', 'mousemove', '500', '370', 'click', '1'], check=True)
    wait_for(lambda: outcome() == 'granted')
    quit()
    launch('regular')
    assert report()['notificationState'] == 'granted', report()
    quit()
    print('Native Allow/Deny controls and prompt choices agree with Chromium queries and persist across restarts: PASS', flush=True)
    profile = launch('private', private=True, permissions=saved)
    assert report()['notificationState'] == 'default' and states()['microphone'] == 'prompt', report()
    request('private-deny')
    key('alt+d')
    wait_for(lambda: outcome() == 'denied')
    for rich in (False, True):
        request('private-clipboard-'+str(rich), 220, rich=rich)
        subprocess.run(['xdotool', 'mousemove', '500', '370', 'click', '1'], check=True)
        wait_for(lambda: outcome() == 'read' and states()['clipboard-read'] == 'prompt')
        assert report()['clipboardValue'] == 'GTK clipboard from native dialog', report()
    print('The actual private-session clipboard dialogs return exact native text/rich data with per-read permissions: PASS', flush=True)
    quit()
    launch('private', private=True)
    assert report()['notificationState'] == 'default', report()
    quit()
    assert (profile/'permissions.ini').read_text() == saved
    print('Private choices ignore the legacy file and disappear on close; regular choices remain unchanged: PASS', flush=True)

except BaseException:
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'failure.png')])
    raise
finally:
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
    if process is not None:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    log.close()
    print('Permission UI artifacts:', root, flush=True)
