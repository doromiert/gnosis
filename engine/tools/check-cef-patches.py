#!/usr/bin/env python3
"""Check the pinned CEF source patch series in a disposable copy, without Git."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile


def digest(data):
    return hashlib.sha256(data).hexdigest()


def relative_path(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or any(part in ('', '.', '..') for part in name.split('/')):
        raise ValueError(f'Invalid source path: {name!r}')
    return Path(*path.parts)


def check(source, patches, report_path=None):
    manifest = json.loads((patches / 'series.json').read_text())
    expected = manifest['sources']
    touched = set()
    verified_patches = []
    for item in manifest['patches']:
        name = item['name']
        if relative_path(name).name != name:
            raise ValueError(f'Invalid patch filename: {name!r}')
        data = (patches / name).read_bytes()
        if digest(data) != item['sha256']:
            raise ValueError(f'Patch changed since its recorded proof: {name}')
        for side, name in re.findall(r'^(---|\+\+\+) ([^\t\n]+)', data.decode(), re.M):
            if name == '/dev/null':
                continue
            prefix = 'a/' if side == '---' else 'b/'
            if not name.startswith(prefix):
                raise ValueError(f'Unexpected patch path: {name!r}')
            touched.add(relative_path(name[2:]).as_posix())
        verified_patches.append((item['name'], data))
    if touched != set(expected):
        raise ValueError('Patch targets differ from the recorded source inventory')

    baseline = {}
    for name, sha in expected.items():
        path = source / relative_path(name)
        if sha is None:
            if path.exists() or path.is_symlink():
                raise ValueError(f'New source target already exists: {name}')
            baseline[name] = None
        else:
            data = path.read_bytes()
            if digest(data) != sha:
                raise ValueError(f'Source does not match the pinned pristine file: {name}')
            baseline[name] = data

    patch_tool = shutil.which('patch')
    if not patch_tool:
        raise ValueError('GNU patch is required')
    with tempfile.TemporaryDirectory(prefix='gnosis-cef-patches-') as temporary:
        root = Path(temporary)
        working = root / 'source'
        working.mkdir()
        for name, data in baseline.items():
            path = working / relative_path(name)
            path.parent.mkdir(parents=True, exist_ok=True)
            if data is not None:
                path.write_bytes(data)
        series = []
        for name, data in verified_patches:
            path = root / name
            path.write_bytes(data)
            series.append(path)
        operations = []
        for reverse in (False, True):
            for path in reversed(series) if reverse else series:
                for dry_run in (True, False):
                    command = [patch_tool, '--batch', '--fuzz=0', '--no-backup-if-mismatch', '-p1',
                               '--directory', str(working), '--input', str(path),
                               '--reverse' if reverse else '--forward']
                    if dry_run:
                        command.append('--dry-run')
                    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, timeout=30)
                    if result.returncode:
                        raise ValueError(f'{path.name} failed ({"reverse" if reverse else "forward"}):\n{result.stdout}')
                    operations.append({'patch': path.name, 'reverse': reverse, 'dry_run': dry_run,
                                       'output': result.stdout})
        for name, data in baseline.items():
            path = working / relative_path(name)
            if data is None:
                if path.exists():
                    raise ValueError(f'Reverse did not remove the added file: {name}')
            elif path.read_bytes() != data:
                raise ValueError(f'Reverse did not restore the original bytes: {name}')
        actual = {p.relative_to(working).as_posix() for p in working.rglob('*') if p.is_file()}
        if actual != {name for name, data in baseline.items() if data is not None}:
            raise ValueError('Patch series left unexpected files after reversal')

    report = {**manifest, 'forward_fuzz_zero': True, 'reverse_fuzz_zero': True,
              'restored_byte_identical': True, 'operations': operations,
              'scope': 'Handwritten source composition only; not SDK generation, compilation or runtime.'}
    if report_path:
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(series)} patches apply and reverse with zero fuzz; '
          f'{len(baseline)} source paths restore exactly.')
    print('SDK generation, compilation and runtime remain separate required checks.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='Pristine pinned CEF directory (the src/cef subtree)')
    parser.add_argument('--report', type=Path, help='Write the checked hashes and operation log as JSON')
    args = parser.parse_args()
    try:
        check(args.source.resolve(), Path(__file__).resolve().parents[1] / 'patches', args.report)
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'FAIL: {error}\n')


if __name__ == '__main__':
    main()
