#!/usr/bin/env python3
"""Check native applications-page icons using disposable desktop entries only."""
import argparse
import base64
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import tempfile
import time
import uuid
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--schemas', type=Path, required=True)
parser.add_argument('--incognito', action='store_true')
args = parser.parse_args()
root = Path(tempfile.mkdtemp(prefix='jupiter-app-icons-ui-', dir='/tmp'))
env = dict(os.environ, XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), GSETTINGS_BACKEND='keyfile',
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()))
subprocess.run(['gsettings', 'set', 'io.github.doromiert.Jupiter', 'ask-for-default', 'false'], env=env, check=True)
base = os.environ['JUPITER_TEST_URL']
desktop_dir = root/'data/applications'
desktop_dir.mkdir(parents=True)
# Epiphany accepts bookmarklets through load_url when the scheme is registered.
(desktop_dir/'icon-test-javascript.desktop').write_text('[Desktop Entry]\nType=Application\n'
    'Name=Disposable bookmarklet fixture\nExec=true %u\nMimeType=x-scheme-handler/javascript;\nNoDisplay=true\n')
(root/'config/mimeapps.list').write_text('[Default Applications]\n'
    'x-scheme-handler/javascript=icon-test-javascript.desktop\n')


def chunk(kind, payload):
    return struct.pack('>I', len(payload))+kind+payload+struct.pack('>I', zlib.crc32(kind+payload))


icon = root/'icon with "quotes".png'
icon.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 32, 8, 2, 0, 0, 0))+
                 chunk(b'IDAT', zlib.compress((b'\0'+bytes((19, 227, 91))*32)*32))+chunk(b'IEND', b''))
text_icon = root/'not-an-image.png'
text_icon.write_text('This is private fixture text, not image data.')
large_icon = root/'too-large.png'
with large_icon.open('wb') as stream:
    stream.write(icon.read_bytes())
    stream.truncate(4*1024*1024+1)
fifo_icon = root/'icon.fifo'
os.mkfifo(fifo_icon)
fixtures = {'pixels': icon, 'text': text_icon, 'large': large_icon,
            'missing': root/'does-not-exist.png', 'fifo': fifo_icon}
for name, path in fixtures.items():
    app_id = 'io.github.doromiert.Jupiter.WebApp_icon-'+name
    profile = root/'data'/app_id
    profile.mkdir()
    (profile/'.app').touch()
    (desktop_dir/(app_id+'.desktop')).write_text('[Desktop Entry]\nType=Application\n'
        f'Name=Icon {name}\nExec={args.binary.resolve()} {base}fixture\nIcon={path}\n')

log = (root/'app.log').open('w')
command = [str(args.binary.resolve()), '--profile='+str(root/'profile')]
if args.incognito:
    command.append('--incognito-mode')
process = subprocess.Popen(command+['about:applications'], env=env,
                           stdout=log, stderr=subprocess.STDOUT, start_new_session=True)


def keys(*values):
    subprocess.run(['xdotool', 'key', '--clearmodifiers', '--delay', '80', *values], check=True)


def navigate(uri):
    keys('ctrl+l')
    subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '0', uri], check=True)
    keys('Return')


def evaluate(source):
    script = 'void(async()=>{'+source+'})()'
    encoded = base64.b64encode(script.encode()).decode()
    navigate("javascript:eval(atob('"+encoded+"'))")


def wait_for(predicate):
    deadline = time.monotonic()+15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((root/'app.log').read_text())
        result = predicate()
        if result:
            return result
        time.sleep(.1)
    raise TimeoutError('Applications icon check did not finish\n'+(root/'app.log').read_text())


def report(name):
    title = subprocess.check_output(['xdotool', 'getwindowname', window], text=True)
    encoded = re.search(r'JupiterIconReport-([A-Za-z0-9+/=]+)', title)
    if not encoded:
        return None
    result = json.loads(base64.b64decode(encoded.group(1)))
    return result if result.get('appIconCheck') == name else None


def send(name, expression):
    return ('document.title="JupiterIconReport-"+btoa(JSON.stringify({appIconCheck:'+
            json.dumps(name)+',value:'+expression+'}));')


def snapshot(name):
    prefix = 'ephy-resource:///org/gnome/epiphany/application-icons/'
    fallback = 'ephy-resource:///org/gnome/epiphany/page-icons/web-app-icon-missing.svg'
    evaluate(send(name, 'Array.from(document.querySelectorAll("tr")).map(row=>{'
        'const img=row.querySelector("img");return [row.id,img.naturalWidth,img.naturalHeight,'
        'img.src==='+json.dumps(fallback)+'?"fallback":img.src.replace('+json.dumps(prefix)+',"")];})'))
    return [{'id': key, 'width': width, 'height': height,
             'src': fallback if source == 'fallback' else prefix+source}
            for key, width, height, source in wait_for(lambda: report(name))['value']]


def denied_source(source, name):
    evaluate('const image=new Image();const result=await new Promise(resolve=>{'
        'image.onload=()=>resolve("loaded");image.onerror=()=>resolve("denied");'
        'setTimeout(()=>resolve("timed-out"),2000);image.src='+json.dumps(source)+';});'+send(name, 'result'))
    assert wait_for(lambda: report(name))['value'] == 'denied', report(name)


try:
    def visible_window():
        result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)], capture_output=True, text=True)
        return result.stdout.splitlines()[-1] if result.returncode == 0 else None
    window = wait_for(visible_window)
    subprocess.run(['xdotool', 'windowfocus', '--sync', window], check=True)
    time.sleep(2)
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'initial.png')], check=True)
    rows = snapshot('initial')
    assert {row['id'] for row in rows} == {'icon-'+name for name in fixtures}, rows
    assert all(row['width'] > 0 and row['height'] > 0 for row in rows), rows
    good = next(row for row in rows if row['id'] == 'icon-pixels')
    prefix = 'ephy-resource:///org/gnome/epiphany/application-icons/'
    assert good['src'].startswith(prefix) and uuid.UUID(good['src'][len(prefix):]), rows
    assert all(row['src'].endswith('/web-app-icon-missing.svg') for row in rows if row != good), rows
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'applications.png')], check=True)
    pixels = subprocess.check_output(['magick', str(root/'applications.png'), '-alpha', 'off', '-depth', '8', 'rgb:-'])
    assert sum(pixels[i:i+3] == bytes((19, 227, 91)) for i in range(0, len(pixels), 3)) > 3500, 'The actual installed-app icon pixels did not render'
    for name, source in {'unknown': prefix+str(uuid.uuid4()), 'path': prefix+'../etc/passwd',
                         'encoded': prefix+'%2e%2e%2fetc%2fpasswd'}.items():
        denied_source(source, name)
    keys('ctrl+r')
    time.sleep(1)
    refreshed = snapshot('refreshed')
    fresh = next(row for row in refreshed if row['id'] == good['id'])
    assert fresh['src'] != good['src'] and fresh['width'] > 0, refreshed
    denied_source(good['src'], 'expired')
    keys('ctrl+t')
    navigate('about:applications')
    time.sleep(1)
    denied_source(fresh['src'], 'other-view')
    keys('ctrl+q')
    assert process.wait(timeout=8) == 0, (root/'app.log').read_text()
    assert all((desktop_dir/('io.github.doromiert.Jupiter.WebApp_icon-'+name+'.desktop')).exists() for name in fixtures)
    print('Installed app pixels render; malformed/oversized/nonregular/missing icons fall back; unknown/path/expired/cross-view capabilities fail; fixtures survive: PASS', flush=True)
except BaseException:
    subprocess.run(['magick', 'import', '-window', 'root', str(root/'failure.png')], check=False)
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
    print('App icon UI artifacts:', root, flush=True)
