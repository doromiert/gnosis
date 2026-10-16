#!/usr/bin/env python3
"""Inside check-native, verify the application's actual GSettings bindings and native preferences dialog."""
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
args = parser.parse_args()
root = Path(tempfile.mkdtemp(prefix='jupiter-preferences-ui-', dir='/tmp'))
env = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), GSETTINGS_BACKEND='keyfile',
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()))
def setting(group, key, value):
    schema = 'io.github.doromiert.Jupiter.'+group
    if group == 'web':
        schema += ':/io/github/doromiert/jupiter/web/'
    subprocess.run(['gsettings', 'set', schema, key, value], env=env, check=True)
setting('web', 'use-gnome-fonts', 'false')
setting('web', 'serif-font', "'DejaVu Serif 15'")
setting('web', 'user-agent', "'JupiterAppFixture/1.0'")
setting('web', 'language', "['fr-FR','fr']")
setting('web', 'chromium-feature-overrides', "{'webgl': false}")
setting('ui', 'webkit-features-page', 'true')
subprocess.run(['gsettings', 'set', 'io.github.doromiert.Jupiter', 'ask-for-default', 'false'], env=env, check=True)
base = os.environ['JUPITER_TEST_URL']
log = (root/'app.log').open('w')
process = subprocess.Popen([str(args.binary.resolve()), '--profile='+str(root/'profile'),
                            base+'settings-fixture?report'], env=env, stdout=log, stderr=subprocess.STDOUT)
def report(agent, size, language):
    deadline = time.monotonic()+20
    reports = []
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((root/'app.log').read_text())
        with urllib.request.urlopen(base+'settings-reports', timeout=2) as response:
            reports = json.load(response)
        matches = [r for r in reports if r['agent']==agent and r['httpUserAgent']==agent and
                   r['size']==size and r['language']==language and r['httpLanguage'].startswith(language)]
        if matches:
            return matches[-1]
        time.sleep(.1)
    raise TimeoutError('Native app preferences did not reach Chromium: '+str(reports[-3:])+'\n'+(root/'app.log').read_text())
try:
    initial = report('JupiterAppFixture/1.0', '20px', 'fr-FR')
    assert initial['initialHeaders']['userAgent']=='JupiterAppFixture/1.0', initial
    assert initial['initialHeaders']['language'].startswith('fr-FR'), initial
    assert 'DejaVu Serif' in initial['family'], initial
    assert not initial['webgl'], initial
    setting('web', 'serif-font', "'DejaVu Serif 21'")
    setting('web', 'user-agent', "'JupiterAppFixture/2.0'")
    setting('web', 'language', "['pl-PL','pl']")
    report('JupiterAppFixture/2.0', '28px', 'pl-PL')
    print('Actual app GSettings control startup and live fonts, identity, language, and restart feature overrides: PASS', flush=True)
    window = subprocess.check_output(['xdotool','search','--onlyvisible','--pid',str(process.pid)],text=True).splitlines()[-1]
    subprocess.run(['xdotool','windowfocus','--sync',window,'key','Escape'],check=True)
    time.sleep(.3)
    subprocess.run(['xdotool','key','F10'],check=True)
    time.sleep(.2)
    subprocess.run(['xdotool','key','e'],check=True)
    time.sleep(.5)
    subprocess.run(['xdotool','key','Tab','Right','Right','Right','space'],check=True)
    time.sleep(.4)
    subprocess.run(['magick','import','-window','root',str(root/'preferences.png')],check=True)
    subprocess.run(['xdotool','key','Tab'],check=True)
    time.sleep(.15)
    subprocess.run(['magick','import','-window','root',str(root/'reset-focus.png')],check=True)
    subprocess.run(['xdotool','key','Return'],check=True)
    deadline = time.monotonic()+3
    while True:
        current = subprocess.check_output(['gsettings','get','io.github.doromiert.Jupiter.web:/io/github/doromiert/jupiter/web/', 'chromium-feature-overrides'],env=env,text=True).strip()
        if current in ('@a{sb} {}','{}') or time.monotonic() >= deadline:
            break
        time.sleep(.05)
    assert current in ('@a{sb} {}','{}'), current
    print('Native feature Reset All writes the real persisted settings: PASS',flush=True)
    subprocess.run(['xdotool','key','Escape'],check=True)
    time.sleep(.2)
    subprocess.run(['xdotool','key','ctrl+q'],check=True)
    assert process.wait(timeout=10)==0, (root/'app.log').read_text()
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait()
    log.close()
    print('Preferences UI artifacts:', root, flush=True)
