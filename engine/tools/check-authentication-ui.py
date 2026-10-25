#!/usr/bin/env python3
"""Exercise the actual GNOME HTTP-auth dialogs against check-native's real server."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--schemas', type=Path, required=True)
parser.add_argument('--clean-app-environment', action='store_true')
parser.add_argument('--proxy-profile-preparer', type=Path,
                    help='Native authentication-test binary that prepares an isolated proxy profile')
args = parser.parse_args()
base = os.environ['JUPITER_TEST_URL']
artifacts = Path(tempfile.mkdtemp(prefix='jupiter-auth-ui-'))
print('HTTP authentication UI artifacts:', artifacts, flush=True)
env = dict(os.environ, GSETTINGS_BACKEND='keyfile', GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()),
           XDG_CONFIG_HOME=str(artifacts/'config'), XDG_CACHE_HOME=str(artifacts/'cache'),
           XDG_DATA_HOME=str(artifacts/'data'), EPHY_LOG_MODULES='ephy-window', LC_ALL='C.UTF-8')
app_env = dict(env)
if args.clean_app_environment:
    for name in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'CEF_ROOT', 'UBLOCK_DIR'):
        app_env.pop(name, None)
browser = None
window = None
log = None
log_path = None


def run(*command):
    return subprocess.check_output(list(map(str, command)), env=env, text=True, timeout=10).strip()


def key(*keys):
    run('xdotool', 'key', '--clearmodifiers', *keys)


def wait(predicate, description):
    deadline = time.monotonic()+25
    while time.monotonic() < deadline:
        if browser and browser.poll() is not None:
            raise RuntimeError('Jupiter exited while waiting for '+description)
        if predicate():
            return
        time.sleep(.1)
    raise TimeoutError(description)


def reports():
    with urllib.request.urlopen(base+'settings-reports', timeout=3) as response:
        return json.load(response)


def observed(realm, authorized):
    return any(r.get('realm') == realm and r.get('authorized') is authorized for r in reports())


def prompts():
    return log_path.read_text().count('Presenting HTTP authentication dialog for ')


def screenshot(name):
    run('magick', 'import', '-window', 'root', artifacts/(name+'.png'))


def launch(kind, realm, private=False, cross=False, profile=None):
    global browser, window, log, log_path
    log_path = artifacts/(realm+'.log')
    log = log_path.open('w')
    profile = profile or artifacts/('private-profile' if private else 'profile')
    command = [str(args.binary.resolve()), '--profile='+str(profile)]
    if private:
        command.append('--incognito-mode')
    origin = ('http://auth-proxy.invalid/' if kind == 'proxy' else
              base.replace('127.0.0.1', 'localhost') if cross else base)
    browser = subprocess.Popen(command+[origin+'auth/'+kind+'?realm='+realm],
                               env=app_env, stdout=log, stderr=subprocess.STDOUT)
    def visible_window():
        result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(browser.pid)],
                                capture_output=True, text=True, env=env)
        return [w for w in result.stdout.splitlines()
                if int(dict(line.split('=', 1) for line in run('xdotool', 'getwindowgeometry', '--shell', w).splitlines())['WIDTH']) > 400]
    wait(visible_window, 'real Jupiter window')
    window = visible_window()[-1]
    run('xdotool', 'windowfocus', '--sync', window)


def challenge(realm):
    wait(lambda: observed(realm, False), 'actual HTTP challenge '+realm)
    wait(lambda: prompts() > 0, 'GNOME authentication dialog '+realm)
    time.sleep(.25)
    screenshot(realm)


def credentials(password='fixture-password'):
    run('xdotool', 'type', '--clearmodifiers', '--delay', 2, 'jupiter-user')
    key('Tab')
    run('xdotool', 'type', '--clearmodifiers', '--delay', 2, password)
    key('Return')


def authenticated(realm):
    wait(lambda: observed(realm, True), 'real credentials accepted by server')
    wait(lambda: 'Authenticated '+realm in run('xdotool', 'getwindowname', window),
         'actual authenticated document commit')


def quit_browser():
    key('ctrl+q')
    assert browser.wait(timeout=15) == 0, 'Jupiter did not quit normally'
    log.close()


try:
    run('gsettings', 'set', 'io.github.doromiert.Jupiter', 'ask-for-default', 'false')
    run('gsettings', 'set', 'io.github.doromiert.Jupiter', 'restore-session-policy', "'crashed'")
    # Test direct protected-page startup. URL-entry completion has its own open
    # regression; do not make credential-dialog assertions depend on typed URLs.
    launch('basic', 'regular-basic')
    challenge('regular-basic')
    previous = prompts()
    credentials('incorrect-password')
    wait(lambda: prompts() > previous, 'real rejected-password retry dialog')
    assert not observed('regular-basic', True)
    time.sleep(.25)
    screenshot('regular-retry')
    credentials()
    authenticated('regular-basic')
    quit_browser()
    launch('digest', 'regular-digest')
    challenge('regular-digest')
    credentials()
    authenticated('regular-digest')
    quit_browser()
    launch('basic', 'regular-cancel', cross=True)
    challenge('regular-cancel')
    key('Escape')
    time.sleep(.4)
    assert not observed('regular-cancel', True), 'Cancellation sent valid credentials'
    quit_browser()
    launch('basic', 'private-basic', private=True)
    challenge('private-basic')
    credentials()
    authenticated('private-basic')
    screenshot('private-authenticated')
    quit_browser()
    if args.proxy_profile_preparer:
        proxy_profile = artifacts/'proxy-profile'
        with (artifacts/'proxy-profile-preparation.log').open('w') as preparation_log:
            subprocess.run([str(args.proxy_profile_preparer.resolve()),
                            '--prepare-proxy-profile='+str(proxy_profile)],
                           env=env, stdout=preparation_log, stderr=subprocess.STDOUT,
                           check=True, timeout=45)
        launch('proxy', 'gui-proxy', profile=proxy_profile)
        challenge('gui-proxy')
        assert '(proxy=1)' in log_path.read_text(), 'Proxy challenge was presented as a server challenge'
        credentials()
        authenticated('gui-proxy')
        screenshot('proxy-authenticated')
        quit_browser()
        print('HTTP_AUTH_PROXY_UI:PASS (persisted native proxy profile, actual 407 dialog, accepted credentials and normal quit)', flush=True)
    print('HTTP_AUTH_UI:PASS (real Basic retry, Digest, cancellation, private dialog, native typing and normal quit)', flush=True)
except BaseException:
    screenshot('failure')
    raise
finally:
    (artifacts/'server-observations.json').write_text(json.dumps(reports(), indent=2)+'\n')
    if browser and browser.poll() is None:
        browser.terminate()
        try:
            browser.wait(timeout=5)
        except subprocess.TimeoutExpired:
            browser.kill(); browser.wait()
    if log and not log.closed:
        log.close()
