#!/usr/bin/env python3
"""Inside check-native's isolated bus/display, exercise the real GTK portal backend."""
import os
import fnmatch
import json
from pathlib import Path
import queue
import re
import signal
import threading
import tempfile
import shutil
import subprocess
import sys
import time

root = Path(os.environ['GNOSIS_TEST_SCRATCH'])
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
native_filter = root / 'native-filter'
native_filter.mkdir()
(native_filter / 'alpha.JUP').write_text('native filter payload')
private_files = root / 'private-files'
private_files.mkdir()
(private_files / 'private.txt').write_text('private folder payload')
folder = root / 'folder'
folder.mkdir()
(folder / 'inside.txt').write_text('folder payload')

processes = []
logs = []

def run_folder_memory(command, monitor_log, monitor_out):
    """Observe real portal options across two independently started Chromium processes."""
    profile = root / 'upload-folder-profile'
    profile.mkdir()
    env = dict(os.environ, GNOSIS_PORTAL_PROFILE=str(profile))
    phases = [
        ('write', [
            ('MEMORY_REGULAR_PICK', Path.home(), native_filter / 'alpha.JUP'),
            ('MEMORY_REGULAR_REUSE', native_filter, None),
            ('MEMORY_PRIVATE_PICK', Path.home(), private_files / 'private.txt'),
            ('MEMORY_PRIVATE_REUSE', private_files, None),
            ('MEMORY_REGULAR_AFTER_PRIVATE', native_filter, None),
            ('MEMORY_EXPLICIT', files, None),
            ('MEMORY_REGULAR_AFTER_CANCEL', native_filter, None),
        ]),
        ('read', [
            ('MEMORY_RESTART_REGULAR', native_filter, None),
            ('MEMORY_RESTART_PRIVATE', Path.home(), None),
        ]),
    ]
    observed = []
    previous_dialog = None
    artifacts = Path(tempfile.mkdtemp(prefix='gnosis-upload-memory-', dir='/tmp'))
    print('UPLOAD_FOLDER_ARTIFACTS ' + str(artifacts), flush=True)
    commands = []
    def xdo(*args, check=True):
        result = subprocess.run(['xdotool', *map(str, args)], text=True,
                                capture_output=True, timeout=5)
        commands.append({'arguments': list(map(str, args)), 'status': result.returncode,
                         'stdout': result.stdout, 'stderr': result.stderr})
        (artifacts / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        if check: result.check_returncode()
        return result.stdout.strip()
    def capture(name):
        subprocess.run(['magick', 'import', '-window', 'root', str(artifacts / (name + '.png'))], check=True)
        monitor_out.flush()
        shutil.copy2(monitor_log, artifacts / 'portal-bus.log')
    def chooser():
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            windows = xdo('search', '--onlyvisible', '--class', 'Xdg-desktop-portal-gtk', check=False)
            candidates = [w for w in windows.splitlines() if w != previous_dialog]
            if candidates:
                return candidates[-1]
            time.sleep(.05)
        raise RuntimeError('The upload-folder portal chooser did not appear')
    for phase, steps in phases:
        arguments = [argument for argument in command if argument not in ('--folder-memory', '--files-only')]
        arguments.insert(1, '--folder-memory-' + phase)
        browser = subprocess.Popen(arguments, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True, bufsize=1)
        processes.append(browser)
        output = queue.Queue()
        def read_output(stream=browser.stdout, destination=output):
            for line in stream:
                print(line, end='', flush=True)
                destination.put(line)
            destination.put(None)
        threading.Thread(target=read_output, daemon=True).start()
        completed = 0
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            try:
                line = output.get(timeout=.25)
            except queue.Empty:
                continue
            if line is None:
                status = browser.wait(timeout=5)
                if status:
                    raise subprocess.CalledProcessError(status, arguments)
                break
            if not line.startswith('PORTAL_') or not line.rstrip().endswith('_READY'):
                continue
            stage = line.strip()[7:-6]
            expected, folder, selected = steps[completed]
            assert stage == expected, (stage, expected)
            dialog = chooser()
            xdo('windowfocus', '--sync', dialog)
            time.sleep(.15)
            capture(stage + '-initial')
            # Read the OpenFile request before changing the location entry. This
            # proves the initial folder came through GTK's actual portal API.
            limit = time.monotonic() + 3
            while True:
                monitor_out.flush()
                calls = monitor_log.read_text().split('member=OpenFile')[1:]
                if len(calls) > len(observed):
                    call = calls[len(observed)].split('\nmethod ', 1)[0]
                    match = re.search(r'string "current_folder"\s+variant\s+array of bytes "([^"]*)"', call)
                    if match:
                        break
                if time.monotonic() >= limit:
                    raise AssertionError('No real current_folder portal option for ' + stage)
                time.sleep(.05)
            actual = match[1]
            assert actual == str(folder), (stage, actual, str(folder))
            observed.append({'stage': stage, 'initial_folder': actual})
            previous_dialog = dialog
            if selected is None:
                xdo('key', 'Escape')
            else:
                xdo('key', '--clearmodifiers', 'ctrl+l')
                time.sleep(.2)
                xdo('key', '--clearmodifiers', 'ctrl+a')
                time.sleep(.05)
                xdo('type', '--clearmodifiers', '--delay', '0', selected)
                time.sleep(.1)
                capture(stage + '-typed')
                xdo('key', 'Return')
                time.sleep(.3)
                if dialog in xdo('search', '--onlyvisible', '--class', 'Xdg-desktop-portal-gtk', check=False).splitlines():
                    capture(stage + '-after-return')
                    xdo('key', 'alt+o')
            completed += 1
        else:
            raise TimeoutError('Upload-folder memory phase timed out: ' + phase)
        assert completed == len(steps), (phase, completed)
    (root / 'upload-folder-memory.json').write_text(json.dumps(observed, indent=2) + '\n')
    (artifacts / 'upload-folder-memory.json').write_text(json.dumps(observed, indent=2) + '\n')
    capture('complete')
    print('Real FileChooser current_folder verifies regular/private memory, explicit path precedence, cancellation and regular-only restart persistence: PASS', flush=True)

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
    processes.append(subprocess.Popen(['stdbuf', '-oL', 'dbus-monitor', '--session', "interface='org.freedesktop.portal.FileChooser'", "interface='org.freedesktop.portal.Request'"], stdout=monitor_out, stderr=subprocess.STDOUT))
    if '--folder-memory' in sys.argv[1:]:
        run_folder_memory(sys.argv[1:], monitor_log, monitor_out)
        raise SystemExit(0)
    artifacts = Path(tempfile.mkdtemp(prefix='gnosis-file-portals-', dir='/tmp'))
    print('FILE_PORTAL_ARTIFACTS ' + str(artifacts), flush=True)
    command_trace = []
    browser = subprocess.Popen(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    processes.append(browser)
    lines = queue.Queue()
    def read_output():
        for line in browser.stdout:
            print(line, end='', flush=True)
            lines.put(line)
        lines.put(None)
    threading.Thread(target=read_output, daemon=True).start()
    expected = ['FILTER', 'SAVE', 'SINGLE', 'MULTIPLE'] + ([] if '--files-only' in sys.argv else ['FOLDER']) + ['CANCEL', 'HIDE', 'CLOSE']
    stages = []
    deadline = time.monotonic() + 65

    def xdo(*args):
        result = subprocess.run(['xdotool', *map(str, args)], text=True,
                                capture_output=True, timeout=5)
        command_trace.append({'arguments': list(map(str, args)), 'status': result.returncode,
                              'stdout': result.stdout, 'stderr': result.stderr})
        (artifacts / 'commands.json').write_text(json.dumps(command_trace, indent=2) + '\n')
        result.check_returncode()
        return result.stdout.strip()

    def capture_stage(name):
        subprocess.run(['magick', 'import', '-window', 'root', str(artifacts / (name + '.png'))], check=True)
        monitor_out.flush()
        shutil.copy2(monitor_log, artifacts / 'portal-bus.log')

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
        subprocess.run(['magick', 'import', '-window', 'root', '/tmp/gnosis-portal-failure.png'])
        raise RuntimeError('The real GTK portal chooser did not appear')

    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=.25)
        except queue.Empty:
            continue
        if line is not None:
            if not line.startswith('PORTAL_') or not line.rstrip().endswith('_READY'):
                continue
            stage = line.strip()[7:-6]
            assert stage == expected[len(stages)], (stage, stages)
            stages.append(stage)
            if stage == 'SINGLE':
                saved = root / 'saved-δ.txt'
                assert saved.read_bytes() == bytes([0, 10, 13, 65, 127, 128, 255, 206, 180]), 'Saved portal bytes changed'
                print('Independently verified the saved UTF-8 file and exact binary contents: PASS', flush=True)
            if stage not in ('FILTER', 'SAVE'):
                # Chromium can signal completion before the portal window unmaps.
                limit = time.monotonic()+5
                while subprocess.run(['xdotool', 'search', '--onlyvisible', '--class', 'Xdg-desktop-portal-gtk'], capture_output=True).returncode == 0:
                    if time.monotonic() >= limit:
                        raise RuntimeError('The previous portal chooser did not close')
                    time.sleep(.05)
            if stage != 'FILTER':
                window = xdo('search', '--onlyvisible', '--name', '^Gnosis Portal Test$').splitlines()[-1]
                xdo('windowfocus', '--sync', window)
                xdo('mousemove', '--window', window, 60, 35)
                xdo('click', 1)
            dialog = chooser()
            if stage in ('HIDE', 'CLOSE'):
                browser.send_signal(signal.SIGUSR1)
                continue
            xdo('windowfocus', '--sync', dialog)
            time.sleep(.15)
            capture_stage(stage + '-initial')
            if stage == 'SAVE':
                xdo('key', '--clearmodifiers', 'ctrl+l')
                time.sleep(.2)
                # GTK already has the real suggested UTF-8 basename. Preserve
                # it instead of synthesizing unmapped Unicode keys through
                # XTest, which can silently lose characters such as delta.
                xdo('key', '--clearmodifiers', 'Home')
                xdo('type', '--clearmodifiers', '--delay', '0', str(root) + '/')
                time.sleep(.1)
                capture_stage(stage + '-typed')
                xdo('key', 'Return')
                continue
            if stage == 'CANCEL':
                xdo('key', 'Escape')
                continue
            xdo('key', '--clearmodifiers', 'ctrl+l')
            time.sleep(.2)
            xdo('key', '--clearmodifiers', 'ctrl+a')
            location = str(native_filter / 'alpha.JUP') if stage == 'FILTER' else str(files / 'alpha.txt')
            if stage == 'MULTIPLE':
                location = str(files) + '/'
            elif stage == 'FOLDER':
                location = str(folder)
            xdo('type', '--clearmodifiers', '--delay', '0', location)
            time.sleep(.1)
            capture_stage(stage + '-typed')
            xdo('key', 'Return')
            time.sleep(.3)
            if stage == 'MULTIPLE':
                xdo('key', 'ctrl+a')
            xdo('key', 'alt+o')
        else:
            status = browser.wait(timeout=5)
            if status:
                subprocess.run(['magick', 'import', '-window', 'root', '/tmp/gnosis-portal-failure.png'])
                raise subprocess.CalledProcessError(status, browser.args)
            break
    else:
        raise TimeoutError('Portal upload tests did not complete')
    assert stages == expected, stages
    monitor_out.flush()
    shutil.copy2(monitor_log, artifacts / 'portal-bus.log')
    traffic = monitor_log.read_text()
    assert traffic.count('member=OpenFile') == len(expected)-1, traffic
    assert traffic.count('member=SaveFile') == 1, traffic
    assert 'member=Response' in traffic, traffic
    assert traffic.count('member=Close') == 2, traffic
    # Inspect the actual first portal request's selected filter, not its title
    # or the permissive "All files" choice. Exercise Chromium's named native
    # filter metadata and GTK's case-insensitive suffix serialization.
    filter_call = traffic.split('member=OpenFile', 1)[1].split('\nmethod ', 1)[0]
    selected_filter = filter_call.split('string "current_filter"', 1)[1].split('dict entry(', 1)[0]
    assert 'string "Gnosis native filter fixture"' in selected_filter, selected_filter
    patterns = re.findall(r'uint32 0\s+string "([^"]*)"', selected_filter)
    for filename in ('alpha.jup', 'alpha.JUP', 'alpha.gnosis', 'alpha.GNOSIS'):
        assert any(fnmatch.fnmatchcase(filename, pattern) for pattern in patterns), (filename, patterns)
    assert not any(fnmatch.fnmatchcase('alpha.txt', pattern) for pattern in patterns), patterns
    print('Verified named Chromium extension rules, upper-case matching and unrelated-type exclusion in the real portal request: PASS')
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
