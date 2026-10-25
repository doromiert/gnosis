#!/usr/bin/env python3
"""Inside check-native, verify actual GTK print portals and the rendered PDF."""
import argparse
import os
from pathlib import Path
import queue
import shlex
import shutil
import signal
import subprocess
import tempfile
import threading
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--pdftotext', default=shutil.which('pdftotext'))
args = parser.parse_args()
if not args.pdftotext:
    parser.error('Supply --pdftotext or put Poppler pdftotext on PATH')
root = Path(tempfile.mkdtemp(prefix='jupiter-print-portals-'))
output = root / 'print-δ.pdf'
env = dict(os.environ, GTK_USE_PORTAL='1', GDK_DEBUG='portals', GTK_PRINT_BACKENDS='file',
           XDG_CONFIG_HOME=str(root/'config'), XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache'), JUPITER_TEST_PRINT_FILE=str(output))
config = root/'config/xdg-desktop-portal'
config.mkdir(parents=True)
(config/'portals.conf').write_text('[preferred]\ndefault=gtk\norg.freedesktop.impl.portal.Print=gtk\n')
gtk_config = root/'config/gtk-3.0'
gtk_config.mkdir()
(gtk_config/'settings.ini').write_text('[Settings]\ngtk-print-backends=file\n')
processes, logs = [], []
print('Print portal artifacts:', root, flush=True)


def xdo(*arguments):
    return subprocess.check_output(['xdotool', *map(str, arguments)], env=env,
                                   text=True, timeout=5).strip()


def screenshot(name):
    subprocess.run(['magick', 'import', '-window', 'root', str(root/name)], env=env, check=True)


def dialog():
    deadline = time.monotonic()+8
    while time.monotonic() < deadline:
        result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--class',
                                 'Xdg-desktop-portal-gtk'], env=env, capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout.splitlines()[-1]
        time.sleep(.05)
    raise TimeoutError('The real GTK print portal dialog did not appear')


try:
    # The backend gets only GTK's virtual file printer; no physical job is possible.
    for name in ('org.freedesktop.impl.portal.desktop.gtk', 'org.freedesktop.portal.Desktop'):
        service = Path('/run/current-system/sw/share/dbus-1/services')/(name+'.service')
        command = next(line[5:] for line in service.read_text().splitlines() if line.startswith('Exec='))
        log = (root/(name+'.log')).open('w'); logs.append(log)
        processes.append(subprocess.Popen(shlex.split(command), env=env, stdout=log, stderr=subprocess.STDOUT))
        subprocess.run(['gdbus', 'wait', '--session', '--timeout=10', name], env=env, check=True)
    deadline = time.monotonic()+10
    while True:
        result = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.portal.Desktop',
                                 '--object-path', '/org/freedesktop/portal/desktop', '--method',
                                 'org.freedesktop.DBus.Properties.Get', 'org.freedesktop.portal.Print',
                                 'version'], env=env, capture_output=True, text=True)
        if result.returncode == 0:
            break
        if time.monotonic() >= deadline:
            raise RuntimeError(result.stderr)
        time.sleep(.1)
    bus_log = (root/'bus.log').open('w'); logs.append(bus_log)
    processes.append(subprocess.Popen(['dbus-monitor', '--session',
        "interface='org.freedesktop.portal.Print'", "interface='org.freedesktop.portal.Request'"],
        env=env, stdout=bus_log, stderr=subprocess.STDOUT))
    browser_log = (root/'native.log').open('w'); logs.append(browser_log)
    browser = subprocess.Popen([str(args.binary.resolve())], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, bufsize=1)
    processes.append(browser)
    lines = queue.Queue()

    def read_output():
        for line in browser.stdout:
            browser_log.write(line); browser_log.flush()
            lines.put(line)
        lines.put(None)

    threading.Thread(target=read_output, daemon=True).start()
    expected = ['SAVE', 'CANCEL', 'HIDE', 'CLOSE']
    ready, passed = [], []
    deadline = time.monotonic()+90
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=.25)
        except queue.Empty:
            continue
        if line is None:
            if browser.wait(timeout=5):
                raise RuntimeError('Native print test failed; see '+str(root/'native.log'))
            break
        print(line, end='', flush=True)
        text = line.strip()
        if text.startswith('PRINT_') and text.endswith('_READY'):
            phase = text[6:-6]
            assert phase == expected[len(ready)], (phase, ready)
            ready.append(phase)
            window = dialog()
            xdo('windowfocus', '--sync', window)
            screenshot(phase.lower()+'.png')
            if phase in ('HIDE', 'CLOSE'):
                browser.send_signal(signal.SIGUSR1)
            elif phase == 'CANCEL':
                geometry = dict(line.split('=', 1) for line in xdo('getwindowgeometry', '--shell', window).splitlines())
                xdo('mousemove', '--window', window, int(geometry['WIDTH'])-144,
                    int(geometry['HEIGHT'])-25, 'click', '1')
            else:
                xdo('key', '--clearmodifiers', 'Return')
        elif text.startswith('PRINT_') and text.endswith('_PASS'):
            phase = text[6:-5]
            assert phase == expected[len(passed)], (phase, passed)
            passed.append(phase)
            if phase == 'SAVE':
                assert output.read_bytes().startswith(b'%PDF-'), 'Portal did not write a PDF'
                extracted = subprocess.check_output([args.pdftotext, str(output), '-'], text=True)
                assert 'Chromium in GTK' in extracted, repr(extracted)
                (root/'pdf-text.txt').write_text(extracted)
                print('PRINT_PDF_CONTENT:PASS (actual Chromium document, UTF-8 destination)', flush=True)
    else:
        raise TimeoutError('Native print workflow did not finish: '+repr(passed))
    assert ready == passed == expected, (ready, passed)
    native_log = (root/'native.log').read_text()
    for failure in ('stack smashing detected', 'Check failed:', 'Diagnostic process failed'):
        assert failure not in native_log, native_log
    bus_log.flush()
    traffic = (root/'bus.log').read_text()
    assert traffic.count('member=PreparePrint') == 4, traffic
    assert traffic.count('member=Print\n') == 1, traffic
    assert traffic.count('member=Close') == 2, traffic
    assert 'member=Response' in traffic, traffic
    assert subprocess.run(['xdotool', 'search', '--onlyvisible', '--class',
                          'Xdg-desktop-portal-gtk'], env=env, capture_output=True).returncode != 0, \
        'Print dialog survived browser closure'
    print('PRINT_DBUS:PASS (four PreparePrint, one real PDF job, two pending-request Close calls)', flush=True)
except BaseException:
    screenshot('failure.png')
    raise
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
    for log in logs:
        log.close()
