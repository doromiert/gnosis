#!/usr/bin/env python3
"""Inside check-native's isolated bus/display, exercise the real GTK portal backend."""
import os
from pathlib import Path
import queue
import signal
import threading
import subprocess
import sys
import time

root = Path(os.environ['JUPITER_TEST_SCRATCH'])
services = Path('/run/current-system/sw/share/dbus-1/services')

def service(name):
    path = services / (name + '.service')
    for line in path.read_text().splitlines():
        if line.startswith('Exec='):
            return line[5:]
    raise RuntimeError(f'No executable in {path}')

config = root / 'config' / 'xdg-desktop-portal'
config.mkdir(parents=True)
(config / 'portals.conf').write_text('[preferred]\ndefault=gtk\norg.freedesktop.impl.portal.FileChooser=gtk\n')
files = root / 'files'
files.mkdir()
(files / 'alpha.txt').write_text('portal payload alpha')
(files / 'beta.txt').write_text('portal payload beta')
folder = root / 'folder'
folder.mkdir()
(folder / 'inside.txt').write_text('folder payload')

processes = []
logs = []
try:
    for name in ('org.freedesktop.impl.portal.desktop.gtk', 'org.freedesktop.portal.Desktop'):
        log = (root / (name + '.log')).open('w')
        logs.append(log)
        processes.append(subprocess.Popen([service(name)], stdout=log, stderr=subprocess.STDOUT))
        subprocess.run(['gdbus', 'wait', '--session', '--timeout=10', name], check=True)
    deadline = time.monotonic() + 10
    while True:
        version = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.portal.Desktop',
            '--object-path', '/org/freedesktop/portal/desktop', '--method', 'org.freedesktop.DBus.Properties.Get',
            'org.freedesktop.portal.FileChooser', 'version'], capture_output=True, text=True)
        if version.returncode == 0:
            break
        if time.monotonic() >= deadline:
            raise RuntimeError('The real FileChooser portal interface did not initialize: ' + version.stderr)
        time.sleep(.1)
    # Observe actual FileChooser D-Bus calls, rather than inferring portal use from GTK.
    monitor_log = root / 'portal-bus.log'
    monitor_out = monitor_log.open('w')
    logs.append(monitor_out)
    processes.append(subprocess.Popen(['dbus-monitor', '--session', "interface='org.freedesktop.portal.FileChooser'", "interface='org.freedesktop.portal.Request'"], stdout=monitor_out, stderr=subprocess.STDOUT))
    browser = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    processes.append(browser)
    lines = queue.Queue()
    def read_output():
        for line in browser.stdout:
            lines.put(line)
        lines.put(None)
    threading.Thread(target=read_output, daemon=True).start()
    expected = ['SAVE', 'SINGLE', 'MULTIPLE'] + ([] if '--files-only' in sys.argv else ['FOLDER']) + ['CANCEL', 'HIDE', 'CLOSE']
    stages = []
    deadline = time.monotonic() + 65

    def xdo(*args):
        return subprocess.check_output(['xdotool', *map(str, args)], text=True, timeout=5).strip()

    def chooser():
        limit = time.monotonic() + 5
        while time.monotonic() < limit:
            try:
                windows = xdo('search', '--onlyvisible', '--class', 'Xdg-desktop-portal-gtk')
                if windows:
                    return windows.splitlines()[-1]
            except subprocess.CalledProcessError:
                pass
            time.sleep(.1)
        subprocess.run(['xdotool', 'search', '--onlyvisible', '--name', '.*', 'getwindowname', '%@'])
        subprocess.run(['magick', 'import', '-window', 'root', '/tmp/jupiter-portal-failure.png'])
        raise RuntimeError('The real GTK portal chooser did not appear')

    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=.25)
        except queue.Empty:
            continue
        if line is not None:
            print(line, end='', flush=True)
            if not line.startswith('PORTAL_') or not line.rstrip().endswith('_READY'):
                continue
            stage = line.strip()[7:-6]
            assert stage == expected[len(stages)], (stage, stages)
            stages.append(stage)
            if stage == 'SINGLE':
                saved = root / 'saved-δ.txt'
                assert saved.read_bytes() == bytes([0, 10, 13, 65, 127, 128, 255, 206, 180]), 'Saved portal bytes changed'
                print('Independently verified the saved UTF-8 file and exact binary contents: PASS', flush=True)
            if stage != 'SAVE':
                # Chromium can signal completion before the portal window unmaps.
                limit = time.monotonic()+5
                while subprocess.run(['xdotool', 'search', '--onlyvisible', '--class', 'Xdg-desktop-portal-gtk'], capture_output=True).returncode == 0:
                    if time.monotonic() >= limit:
                        raise RuntimeError('The previous portal chooser did not close')
                    time.sleep(.05)
            window = xdo('search', '--onlyvisible', '--name', '^Jupiter Portal Test$').splitlines()[-1]
            xdo('windowfocus', '--sync', window)
            xdo('mousemove', '--window', window, 60, 35)
            xdo('click', 1)
            dialog = chooser()
            if stage in ('HIDE', 'CLOSE'):
                browser.send_signal(signal.SIGUSR1)
                continue
            xdo('windowfocus', '--sync', dialog)
            if stage == 'SAVE':
                xdo('key', '--clearmodifiers', 'ctrl+l')
                xdo('key', '--clearmodifiers', 'ctrl+a')
                xdo('type', '--clearmodifiers', '--delay', '1', str(root / 'saved-δ.txt'))
                xdo('key', 'Return')
                continue
            if stage == 'CANCEL':
                xdo('key', 'Escape')
                continue
            xdo('key', '--clearmodifiers', 'ctrl+l')
            location = str(files / 'alpha.txt')
            if stage == 'MULTIPLE':
                location = str(files) + '/'
            elif stage == 'FOLDER':
                location = str(folder)
            xdo('type', '--clearmodifiers', '--delay', '1', location)
            xdo('key', 'Return')
            time.sleep(.3)
            if stage == 'MULTIPLE':
                xdo('key', 'ctrl+a')
            xdo('key', 'alt+o')
        else:
            status = browser.wait(timeout=5)
            if status:
                subprocess.run(['magick', 'import', '-window', 'root', '/tmp/jupiter-portal-failure.png'])
                raise subprocess.CalledProcessError(status, browser.args)
            break
    else:
        raise TimeoutError('Portal upload tests did not complete')
    assert stages == expected, stages
    monitor_out.flush()
    traffic = monitor_log.read_text()
    assert traffic.count('member=OpenFile') == len(expected)-1, traffic
    assert traffic.count('member=SaveFile') == 1, traffic
    assert 'member=Response' in traffic, traffic
    assert traffic.count('member=Close') == 2, traffic
    assert subprocess.run(['xdotool', 'search', '--onlyvisible', '--class', 'Xdg-desktop-portal-gtk'], capture_output=True).returncode != 0, 'Chooser survived browser closure'
    print(f'Verified {len(expected)} native FileChooser portal calls, cancellation, and pending request closure on D-Bus: PASS')
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    for log in logs:
        log.close()
    for log in root.glob('*.log'):
        print(f'--- {log.name} ---\n{log.read_text()}', flush=True)
