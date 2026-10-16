#!/usr/bin/env python3
"""Exercise native Chromium results through the actual Find and Reader UI."""
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
root = Path(tempfile.mkdtemp(prefix='jupiter-script-ui-', dir='/tmp'))
env = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), GSETTINGS_BACKEND='keyfile',
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()))
subprocess.run(['gsettings', 'set', 'io.github.doromiert.Jupiter', 'ask-for-default', 'false'], env=env, check=True)
base = os.environ['JUPITER_TEST_URL']
log = (root/'app.log').open('w')
scope = 'private' if args.incognito else 'regular'
command = [str(args.binary.resolve()), '--profile='+str(root/'profile')]
if args.incognito:
    command.append('--incognito-mode')
process = subprocess.Popen(command+[base+'script-ui-fixture?scope='+scope],
                           env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

def key(*keys):
    subprocess.run(['xdotool', 'key', '--delay', '80', *keys], check=True)

def click(x, y):
    subprocess.run(['xdotool', 'mousemove', str(x), str(y), 'click', '1'], check=True)

def toggle_reader():
    click(302, 30)
    time.sleep(.3)
    click(265, 126)

def text():
    with urllib.request.urlopen(base+'settings-reports', timeout=2) as response:
        reports = [r for r in json.load(response) if r.get('scriptUI')]
    return reports[-1]['text'] if reports else None

def wait_for(predicate):
    deadline = time.monotonic()+15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((root/'app.log').read_text())
        if predicate():
            return
        time.sleep(.1)
    raise TimeoutError('Script UI did not finish: '+repr(text())+'\n'+(root/'app.log').read_text())

try:
    wait_for(lambda: text() is not None)
    window = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)], text=True).splitlines()[-1]
    subprocess.run(['xdotool', 'windowfocus', '--sync', window], check=True)
    click(160, 100)
    key('ctrl+f')
    time.sleep(.6)
    key('ctrl+a', 'ctrl+c', 'Escape')
    click(160, 180)
    key('ctrl+v')
    wait_for(lambda: text() == 'Jupiter native results')
    print('The actual GTK Find entry receives Chromium selection text; copying and pasting it returns the exact result: PASS', flush=True)
    time.sleep(2)
    key('ctrl+alt+r')
    time.sleep(5)
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'reader.png')], check=True)
    click(600, 300)
    key('ctrl+a', 'ctrl+c')
    key('ctrl+alt+r')
    time.sleep(.8)
    click(160, 180)
    key('ctrl+a', 'ctrl+v')
    wait_for(lambda: text() is not None and 'article parser' in text())
    assert 'Native result article' in text() and 'Select phrase' not in text() and 'Paste proof' not in text(), text()
    assert 'Live DOM edit preserved by reader extraction' in text(), text()
    print('The real Reader Mode renders Chromium article metadata/content and strips fixture controls; toggling back and native paste preserve the article: PASS', flush=True)
    key('ctrl+l')
    subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '1', 'ephy-reader:'+base+'script-ui-fixture?scope='+scope], check=True)
    key('Return')
    time.sleep(5)
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'cold-reader.png')], check=True)
    click(600, 300)
    key('ctrl+a', 'ctrl+c')
    toggle_reader()
    time.sleep(.8)
    click(160, 180)
    key('ctrl+a', 'ctrl+v')
    wait_for(lambda: text() is not None and 'Reader session cookie: reader_scope='+scope in text())
    assert 'Select phrase' not in text() and 'Live DOM edit preserved by reader extraction' not in text(), text()
    print('An explicit reader URL loads fresh source in the correct '+scope+' Chromium session and releases its hidden view on close: PASS', flush=True)
    key('ctrl+l')
    subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '1', 'ephy-reader:'+base+'script-ui-fixture?scope='+scope+'&slow=1'], check=True)
    key('Return')
    def slow_requested():
        with urllib.request.urlopen(base+'settings-reports', timeout=2) as response:
            return any(r.get('readerSlowRequested') for r in json.load(response))
    wait_for(slow_requested)
    key('ctrl+q')
    assert process.wait(timeout=5) == 0, (root/'app.log').read_text()
    print('Quit during an actual pending reader source request exits without waiting for the server: PASS', flush=True)
except BaseException:
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'failure.png')])
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
    print('Script UI artifacts:', root, flush=True)
