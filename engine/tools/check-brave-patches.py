#!/usr/bin/env python3
"""Check Gnosis's small Brave patch series without changing a checkout."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.request


def digest(data):
    return hashlib.sha256(data).hexdigest()


def relative(name):
    path = PurePosixPath(name)
    if not name or path.is_absolute() or any(p in ('', '.', '..') for p in name.split('/')):
        raise ValueError(f'Unsafe patch path: {name!r}')
    return Path(*path.parts)


def versions(metadata):
    brave = metadata['version']
    chromium = metadata['config']['projects']['chrome']['tag']
    if not re.fullmatch(r'\d+\.\d+\.\d+', brave) or not re.fullmatch(r'\d+\.\d+\.\d+\.\d+', chromium):
        raise ValueError('Brave metadata does not identify release versions')
    return brave, chromium


def download(url, optional=False):
    request = urllib.request.Request(url, headers={'User-Agent': 'Gnosis-patch-check/1'})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read(16 * 1024 * 1024 + 1)
    except urllib.error.HTTPError as error:
        if optional and error.code == 404:
            return None
        raise
    if len(data) > 16 * 1024 * 1024:
        raise ValueError('Source response exceeded the download limit')
    return data


def fetch_source(root, brave, manifest):
    if not re.fullmatch(r'\d+\.\d+\.\d+', brave):
        raise ValueError('Use a numeric Brave release version')
    base = f'https://raw.githubusercontent.com/brave/brave-core/v{brave}/'
    metadata = download(base + 'package.json')
    actual, chromium = versions(json.loads(metadata))
    if actual != brave:
        raise ValueError('Release metadata version does not match its tag')
    path = root / 'brave/package.json'
    path.parent.mkdir(parents=True)
    path.write_bytes(metadata)
    for name in manifest['sources']:
        relative(name)
        # A new Brave override needs review against the initialized checkout;
        # checking bare Chromium in that situation would give false confidence.
        probes = ['patches/' + name.replace('/', '-') + '.patch',
                  'chromium_src/' + name,
                  'chromium_src/' + str(PurePosixPath(name).with_suffix('.h'))]
        for probe in probes:
            if download(base + probe, optional=True) is not None:
                raise ValueError(f'Brave customizes {name} via {probe}; check its initialized src checkout instead')
        path = root / relative(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(download(f'https://raw.githubusercontent.com/chromium/chromium/{chromium}/{name}'))


def check(source, patches, upgrade=False):
    manifest = json.loads((patches / 'series.json').read_text())
    if manifest.get('schema') != 1:
        raise ValueError('Unsupported patch manifest schema')
    metadata = (source / 'brave/package.json').read_bytes()
    brave, chromium = versions(json.loads(metadata))
    if not upgrade and (brave, chromium) != (manifest['brave_version'], manifest['chromium_version']):
        raise ValueError('Release pair differs from the pin; use --upgrade for a candidate check')
    if not upgrade and digest(metadata) != manifest['metadata_sha256']:
        raise ValueError('Pinned Brave release metadata changed; use --upgrade to review the candidate')
    baseline, series, touched = {}, [], set()
    for item in manifest['patches']:
        name = item['file']
        if relative(name).name != name:
            raise ValueError(f'Patch must be a filename: {name}')
        data = (patches / name).read_bytes()
        if digest(data) != item['sha256']:
            raise ValueError(f'Patch hash changed: {name}; review and update the manifest')
        for side, target in re.findall(r'^(---|\+\+\+) ([^\t\n]+)', data.decode(), re.M):
            if target == '/dev/null':
                raise ValueError('This series only patches existing integration points')
            prefix = 'a/' if side == '---' else 'b/'
            if not target.startswith(prefix):
                raise ValueError(f'Invalid patch header: {target}')
            touched.add(relative(target[2:]).as_posix())
        series.append((name, data))
    if not series or touched != set(manifest['sources']):
        raise ValueError('Patch targets do not match the source inventory')
    for name, expected in manifest['sources'].items():
        data = (source / relative(name)).read_bytes()
        if not upgrade and digest(data) != expected:
            raise ValueError(f'Pinned source changed: {name}; use --upgrade to check upstream drift')
        baseline[name] = data
    tool = shutil.which('patch')
    if not tool:
        raise ValueError('GNU patch is required')
    operations = []
    with tempfile.TemporaryDirectory(prefix='gnosis-brave-patches-') as directory:
        root = Path(directory)
        working = root / 'src'
        for name, data in baseline.items():
            path = working / relative(name)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        inputs = []
        for name, data in series:
            path = root / name
            path.write_bytes(data)
            inputs.append(path)
        for reverse in (False, True):
            for path in reversed(inputs) if reverse else inputs:
                for dry in (True, False):
                    command = [tool, '--batch', '--fuzz=0', '--no-backup-if-mismatch', '-p1',
                               '--directory', str(working), '--input', str(path),
                               '--reverse' if reverse else '--forward']
                    if dry:
                        command.append('--dry-run')
                    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            text=True, timeout=30)
                    if result.returncode:
                        raise ValueError(f'{path.name} cannot {"reverse" if reverse else "apply"}:\n{result.stdout}')
                    operations.append({'patch': path.name, 'reverse': reverse, 'dry_run': dry,
                                       'output': result.stdout.strip()})
        actual = {p.relative_to(working).as_posix() for p in working.rglob('*') if p.is_file()}
        if actual != set(baseline) or any((working / n).read_bytes() != data for n, data in baseline.items()):
            raise ValueError('Reverse application did not restore the original files byte for byte')
    candidate = {**manifest, 'brave_version': brave, 'chromium_version': chromium,
                 'metadata_sha256': digest(metadata),
                 'sources': {name: digest(data) for name, data in baseline.items()}}
    return {'mode': 'upgrade-candidate' if upgrade else 'pinned', 'brave_version': brave,
            'chromium_version': chromium, 'zero_fuzz': True, 'reverse_byte_identical': True,
            'operations': operations, 'candidate_manifest': candidate,
            'scope': 'Source application only. Native Brave compilation and graphical runtime remain unverified.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', nargs='?', type=Path, help='Initialized Brave Chromium src directory')
    parser.add_argument('--fetch', action='store_true', help='Download only the affected release sources')
    parser.add_argument('--brave-version', help='Release candidate for --fetch; defaults to the pin')
    parser.add_argument('--upgrade', action='store_true', help='Allow source/version drift, never patch drift or fuzz')
    parser.add_argument('--patches', type=Path, default=Path(__file__).resolve().parents[1] / 'brave/patches')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--candidate-pin', type=Path, help='Write a proposed pin separately; never change the real pin')
    args = parser.parse_args()
    if bool(args.source) == args.fetch or (args.brave_version and not args.fetch):
        parser.error('Choose either a src directory or --fetch; --brave-version requires --fetch')
    pin = (args.patches / 'series.json').resolve()
    if any(output and output.resolve() == pin for output in (args.report, args.candidate_pin)):
        parser.error('Reports and proposed pins must not overwrite the real series.json')
    try:
        with tempfile.TemporaryDirectory(prefix='gnosis-brave-source-') as directory:
            source = args.source.resolve() if args.source else Path(directory)
            if args.fetch:
                manifest = json.loads((args.patches / 'series.json').read_text())
                fetch_source(source, args.brave_version or manifest['brave_version'], manifest)
            report = check(source, args.patches, args.upgrade)
        if args.report:
            args.report.write_text(json.dumps(report, indent=2) + '\n')
        if args.candidate_pin:
            args.candidate_pin.write_text(json.dumps(report['candidate_manifest'], indent=2) + '\n')
        print(f"PASS ({report['mode']}): Brave {report['brave_version']} / Chromium {report['chromium_version']}; "
              'patches apply with zero fuzz and reverse byte-identically.')
        print(report['scope'])
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'FAIL: {error}\n')


if __name__ == '__main__':
    main()
