#!/usr/bin/env python3
"""Check association sharing through real Gnosis/extension workers using public fixture keys.

The preload blocks native-host discovery, so no real KeePassXC database is used.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import resource
import shlex
import signal
import subprocess
import tempfile
import threading
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--browser', required=True, type=Path)
parser.add_argument('--probe', required=True, type=Path)
parser.add_argument('--xvfb', default='Xvfb')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
reports, lock = {}, threading.Lock()

class Fixture(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_GET(self):
        self.send_response(200); self.send_header('Content-Type', 'text/html'); self.end_headers()
        self.wfile.write(b'<!doctype html><title>Pairing fixture</title><p>Native association sharing</p>')
    def do_POST(self):
        value = json.loads(self.rfile.read(min(8192, int(self.headers.get('Content-Length', '0')))))
        with lock: reports[value['role']] = value
        self.send_response(204); self.send_header('Access-Control-Allow-Origin', '*'); self.end_headers()

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
threading.Thread(target=server.serve_forever, daemon=True).start()
with tempfile.TemporaryDirectory(prefix='gnosis-pairing-native-') as scratch:
    root = Path(scratch); data = root/'data'; data.mkdir()
    phase = root/'phase'; phase.write_text('seed')
    bus = root/'bus.conf'
    bus.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
    display_file = root/'display'
    with display_file.open('w') as output:
        display = subprocess.Popen([args.xvfb, '-displayfd', str(output.fileno()), '-screen', '0', '1200x900x24', '-nolisten', 'tcp', '-ac'], pass_fds=(output.fileno(),), stderr=subprocess.DEVNULL)
    processes = []
    # Owned launchers follow the current build; declarative launchers stay managed.
    applications = data/'applications'; applications.mkdir()
    launchers = {}
    for name, managed in [('owned', False), ('managed', True)]:
        profile = data/('com.neg-zero.Gnosis.WebApp_'+name); profile.mkdir(); (profile/'.app').touch()
        alias = root/('old-'+name); alias.symlink_to(args.browser.resolve())
        launcher = applications/(profile.name+'.desktop')
        launcher.write_text('[Desktop Entry]\nType=Application\nName=Launcher fixture\nIcon=folder\n'+
            ('X-Gnosis-Declarative=true\n' if managed else '')+
            'Exec='+str(alias)+' --application-mode --profile='+str(profile)+' --fixture-argument http://example.test/\n')
        launchers[name] = (launcher, launcher.read_text())
    def wait(predicate, timeout=35):
        until = time.monotonic()+timeout
        while time.monotonic()<until:
            for process, log in processes:
                if process.poll() is not None: raise RuntimeError(log.read_text()[-6000:])
            value = predicate()
            if value: return value
            time.sleep(.1)
        raise TimeoutError('KeePassXC pairing fixture did not complete: '+json.dumps(reports))
    def launch(role):
        profile = data/'gnosis' if role in ('main', 'private') else data/('com.neg-zero.Gnosis.WebApp_'+role)
        arguments = []
        if role in ('app', 'late', 'disabled'):
            profile.mkdir(exist_ok=True); (profile/'.app').touch()
            (profile/'web-app.ini').write_text('[Extensions]\nenabled='+('keepassxc;' if role!='disabled' else '')+'\n')
            applications = data/'applications'; applications.mkdir(exist_ok=True)
            (applications/(profile.name+'.desktop')).write_text('[Desktop Entry]\nType=Application\nName=Pairing fixture\nIcon=folder\nX-Gnosis-Declarative=true\nExec='+shlex.quote(str(args.browser.resolve()))+' --application-mode --profile='+str(profile)+' http://127.0.0.1:'+str(server.server_port)+'/\n')
            arguments += ['--application-mode', '--profile='+str(profile)]
        elif role=='private': arguments += ['--incognito-mode']
        elif role=='custom': arguments += ['--profile='+str(profile)]
        env = dict(os.environ, DISPLAY=':'+display_file.read_text().strip(), GDK_BACKEND='x11', GSK_RENDERER='cairo', GTK_A11Y='none', GSETTINGS_BACKEND='memory',
                   XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(data), XDG_CACHE_HOME=str(root/'cache'),
                   LD_PRELOAD=str(args.probe.resolve()), GNOSIS_PAIRING_PROBE_ROLE=role, GNOSIS_PAIRING_PROBE_ROOT=str(root), GNOSIS_PAIRING_PROBE_REPORT=f'http://127.0.0.1:{server.server_port}/report')
        for name in ('WAYLAND_DISPLAY', 'LD_LIBRARY_PATH', 'GI_TYPELIB_PATH', 'GTK_PATH'): env.pop(name, None)
        log = root/(role+'.log')
        with log.open('w') as output:
            process = subprocess.Popen(['dbus-run-session', '--config-file='+str(bus), '--', str(args.browser.resolve()), *arguments, f'http://127.0.0.1:{server.server_port}/'], env=env, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        processes.append((process, log))
        return process
    def observed(role, expected):
        with lock: value = reports.get(role, {}).copy()
        assert not value.get('error'), 'Worker fixture failed'
        return value if value.get('phase')==phase.read_text() and value.get('id')==expected and value.get('storedId')==expected else None
    try:
        wait(lambda: display_file.exists() and display_file.read_text().strip())
        main = launch('main'); wait(lambda: observed('main', 'connected'))
        owned = launchers['owned'][0].read_text()
        assert str(args.browser.resolve()) in owned and '--fixture-argument http://example.test/' in owned
        assert launchers['managed'][0].read_text()==launchers['managed'][1]
        shared = data/'gnosis/keepassxc-associations.json'
        wait(lambda: shared.exists() and 'connected' in shared.read_text())
        assert shared.stat().st_mode & 0o777 == 0o600
        launch('app')
        app = wait(lambda: observed('app', 'connected'))
        assert app['mainSentinel'] is None and app['own']=='app-only' and app['appSentinel']=='app-only', app
        phase.write_text('rotate'); wait(lambda: observed('main', 'updated'))
        wait(lambda: observed('app', 'updated'))
        launch('custom'); wait(lambda: observed('custom', 'custom'))
        assert 'updated' in shared.read_text() and 'custom' not in shared.read_text()
        launch('private'); launch('disabled')
        wait(lambda: (root/'private.json').exists() and (root/'disabled.json').exists())
        phase.write_text('clear'); wait(lambda: observed('main', None))
        app = wait(lambda: observed('app', None)); assert app['own']=='app-only'
        wait(lambda: shared.read_text()=='{}')
        phase.write_text('rotate'); wait(lambda: observed('main', 'updated')); wait(lambda: observed('app', 'updated'))
        (root/'stop-main').touch(); assert main.wait(timeout=15)==0
        processes.remove((main, root/'main.log'))
        launch('late'); late = wait(lambda: observed('late', 'updated'))
        assert late['mainSentinel'] is None and late['own']=='late-only' and late['appSentinel']=='late-only'
        print('KEEPASSXC_NATIVE_PAIRING_PASS real worker export/import, live updates and revocation, app storage isolation, private/disabled/custom exclusion, owned launcher upgrade, app launch with main browser closed', flush=True)
    finally:
        phase.write_text('quit')
        for process, _ in processes:
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired: pass
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            process.wait()
        display.terminate(); display.wait(); server.shutdown()
