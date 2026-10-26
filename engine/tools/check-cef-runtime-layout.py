#!/usr/bin/env python3
"""Check a linked Nix CEF compatibility runtime without changing its SDK."""
import argparse
import os
from pathlib import Path
import stat
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('sdk', type=Path)
    args = parser.parse_args()
    package = args.package.resolve(strict=True)
    sdk = args.sdk.resolve(strict=True)
    runtime = package / 'lib/gnosis/cef'
    for directory in ['Release', 'Resources']:
        for original in (sdk / directory).iterdir():
            target = runtime / original.name
            if not target.is_symlink() or target.resolve() != original.resolve():
                raise RuntimeError(f'Runtime entry does not reference the SDK: {target}')
            if stat.S_IMODE(target.stat().st_mode) != stat.S_IMODE(original.stat().st_mode):
                raise RuntimeError(f'Runtime mode changed: {target}')
    if not (runtime / 'icudtl.dat').is_file() or not (runtime / 'locales/en-US.pak').is_file():
        raise RuntimeError('ICU/locale data is missing beside libcef')
    if not os.access(runtime / 'chrome-sandbox', os.X_OK):
        raise RuntimeError('The original sandbox helper is not executable')
    # Observe the actual installed ELF loader, not an inferred RPATH result.
    # A temporary file keeps its large dependency trace out of tool output.
    with tempfile.TemporaryFile(mode='w+') as trace:
        env = dict(os.environ, LD_DEBUG='libs')
        result = subprocess.run([str(package / 'bin/gnosis'), '--help'], env=env,
                                stdout=subprocess.PIPE, stderr=trace, text=True, check=True)
        trace.seek(0)
        loaded = [line for line in trace if 'calling init:' in line and '/libcef.so' in line]
    if not loaded or not any(str(runtime / 'libcef.so') in line for line in loaded):
        raise RuntimeError('The installed executable did not load its linked compatibility runtime')
    if 'gnosis' not in result.stdout:
        raise RuntimeError('The installed executable did not print its help')
    print('CEF_RUNTIME_LAYOUT_PASS SDK references/modes, ICU/locales, original sandbox helper and actual ELF loading')


if __name__ == '__main__':
    main()
