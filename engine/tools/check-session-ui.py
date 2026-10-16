#!/usr/bin/env python3
"""Inside check-native, reopen a real browser session with deferred tabs twice."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import urllib.request
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--schemas', type=Path, required=True)
args = parser.parse_args()
root = Path(tempfile.mkdtemp(prefix='jupiter-session-ui-', dir='/tmp'))
env = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), GSETTINGS_BACKEND='keyfile',
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()))
for key, value in [('ask-for-default', 'false'), ('restore-session-policy', "'always'"),
                   ('restore-session-delaying-loads', 'true')]:
    subprocess.run(['gsettings', 'set', 'io.github.doromiert.Jupiter', key, value], env=env, check=True)
base = os.environ['JUPITER_TEST_URL']
subprocess.run(['gsettings', 'set', 'io.github.doromiert.Jupiter', 'content-filters',
                "['"+base+"obsolete-filter-fixture']"], env=env, check=True)
urls = [base+'session-fixture?'+str(i) for i in range(1, 4)]
log = (root/'app.log').open('w')
process = None

def key(*keys):
    subprocess.run(['xdotool', 'key', *keys], check=True)

def reports():
    with urllib.request.urlopen(base+'settings-reports', timeout=2) as response:
        return Counter(r.get('tab', 'legacy-filter') for r in json.load(response) if 'tab' in r or r.get('legacyFilter'))

def wait_for(predicate):
    deadline = time.monotonic()+15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((root/'app.log').read_text())
        if predicate():
            return
        time.sleep(.1)
    raise TimeoutError('Session restoration failed: '+str(reports())+'\n'+(root/'app.log').read_text())

def launch(arguments):
    global process
    process = subprocess.Popen([str(args.binary.resolve()), '--profile='+str(root/'profile'), *arguments],
                               env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    wait_for(lambda: subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0)
    window = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)], text=True).splitlines()[-1]
    subprocess.run(['xdotool', 'windowfocus', '--sync', window], check=True)

def quit_and_check():
    key('ctrl+q')
    assert process.wait(timeout=10) == 0, (root/'app.log').read_text()
    tabs = ET.parse(root/'profile/session_state.xml').findall('.//embed')
    assert [tab.attrib['url'] for tab in tabs] == urls, [tab.attrib for tab in tabs]
    assert all(not tab.attrib.get('loading') for tab in tabs), [tab.attrib for tab in tabs]
    assert reports()['legacy-filter'] == 0, 'The unused WebKit filter service still downloads lists'

try:
    launch(urls)
    wait_for(lambda: all(reports()['?'+str(i)] == 1 for i in range(1, 4)))
    key('alt+3')
    time.sleep(.3)
    quit_and_check()
    before = reports()
    launch([])
    wait_for(lambda: reports()['?3'] > before['?3'])
    time.sleep(.5)
    assert reports()['?1'] == before['?1'] and reports()['?2'] == before['?2'], reports()
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'deferred.png')], check=True)
    quit_and_check()
    print('Reopening restores the selected real page and saves untouched deferred tabs with logical URLs: PASS', flush=True)
    before = reports()
    launch([])
    wait_for(lambda: reports()['?3'] > before['?3'])
    key('alt+1')
    wait_for(lambda: reports()['?1'] > before['?1'])
    key('alt+2')
    wait_for(lambda: reports()['?2'] > before['?2'])
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'restored.png')], check=True)
    quit_and_check()
    print('A second reopen and selecting deferred tabs load their actual content; normal shutdown passes: PASS', flush=True)
    print('The browser does not start obsolete WebKit filter downloads: PASS', flush=True)
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
    print('Session UI artifacts:', root, flush=True)
