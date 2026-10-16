#!/usr/bin/env python3
"""Inside check-native's isolated display, check the actual browser's download UI and normal shutdown."""
import argparse
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
parser.add_argument('--schemas', type=Path, help='Compiled schemas for an uninstalled build')
args = parser.parse_args()
root = Path(tempfile.mkdtemp(prefix='jupiter-download-ui-', dir='/tmp'))
config = root / 'config'
config.mkdir()
downloads = root / 'Downloads'
downloads.mkdir()
(config / 'user-dirs.dirs').write_text(f'XDG_DOWNLOAD_DIR="{downloads}"\n')
env = dict(os.environ, XDG_CONFIG_HOME=str(config), XDG_CACHE_HOME=str(root / 'cache'),
           XDG_DATA_HOME=str(root / 'data'), GSETTINGS_BACKEND='memory')
if args.schemas:
    env['GSETTINGS_SCHEMA_DIR'] = str(args.schemas.resolve())
def no_memory_cores():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    Path('/proc/self/coredump_filter').write_text('0')
log = (root / 'app.log').open('w')
process = subprocess.Popen([str(args.binary.resolve()), '--profile=' + str(root / 'profile'),
                            os.environ['JUPITER_TEST_URL'] + 'download-slow', os.environ['JUPITER_TEST_URL'] + 'download'], env=env, stdout=log,
                            stderr=subprocess.STDOUT, preexec_fn=no_memory_cores, start_new_session=True)
try:
    deadline = time.monotonic() + 30
    while len(list(downloads.glob('*.bin'))) < 2:
        if process.poll() is not None:
            raise RuntimeError('Jupiter exited before downloading: ' + (root / 'app.log').read_text())
        if time.monotonic() >= deadline:
            raise TimeoutError('Download did not reach Epiphany manager: ' + (root / 'app.log').read_text())
        time.sleep(.1)
    assert {p.name for p in downloads.glob('*.bin')} == {'cef-fixture.bin', 'cef-fixture(1).bin'}
    assert {p.read_bytes() for p in downloads.glob('*.bin')} == {b'CEF download payload\x00with binary bytes', b'x' * (512 * 1024)}
    window = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--pid', str(process.pid)], text=True).splitlines()[-1]
    subprocess.run(['xdotool', 'windowfocus', '--sync', window, 'key', '--clearmodifiers', 'Escape'], check=True)
    time.sleep(.4)
    subprocess.run(['xdotool', 'key', '--clearmodifiers', 'ctrl+shift+y'], check=True)
    time.sleep(.5)
    subprocess.run(['magick', 'import', '-window', 'root', str(root / 'downloads.png')], check=True)
    subprocess.run(['xdotool', 'key', '--clearmodifiers', 'Escape'], check=True)
    time.sleep(.3)
    subprocess.run(['xdotool', 'key', '--clearmodifiers', 'ctrl+q'], check=True)
    try:
        status = process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        subprocess.run(['magick', 'import', '-window', 'root', str(root / 'close-failure.png')], check=True)
        raise
    assert status == 0, (status, (root / 'app.log').read_text())
    print('Actual Epiphany manager handles concurrent downloads with unique names, sandbox enabled, normal exit: PASS', flush=True)
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    log.close()
    print('UI artifacts:', root, flush=True)
