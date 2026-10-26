#!/usr/bin/env python3
"""Inspect shipped ELF requirements without loading code. No support certification."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

MANIFEST = Path(__file__).resolve().parents[2] / 'packaging/release/support-profiles.json'
FAMILIES = ('GLIBC', 'GLIBCXX', 'CXXABI')


def numeric(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9]+(?:\.[0-9]+)+', value):
        raise ValueError(f'Invalid numeric symbol version: {value}')
    parts = tuple(map(int, value.split('.')))
    while len(parts) > 1 and parts[-1] == 0:
        parts = parts[:-1]
    return parts


def profile_contract(name, manifest=MANIFEST):
    document = json.loads(manifest.read_text())
    if document.get('schemaVersion') != 1 or name not in document['profiles']:
        raise ValueError('Unknown support profile/schema')
    return document['profiles'][name]


def parse_needs(text):
    """Read requirements, never the version definitions exported by a library."""
    active = False
    saw_section = False
    names = set()
    expected = actual = 0
    expected_files = actual_files = 0
    for line in text.splitlines():
        if line.startswith('Version ') and ' section ' in line:
            active = line.startswith('Version needs section ')
            saw_section = True
            if active:
                count = re.search(r"contains (\d+) entr", line)
                if not count:
                    raise ValueError('Unrecognized version-needs header')
                expected_files += int(count[1])
        if active:
            count = re.search(r'\bFile:.*\bCnt:\s+(\d+)', line)
            if count:
                expected += int(count[1])
                actual_files += 1
            name = re.search(r'\bName:\s+(\S+)', line)
            if name:
                names.add(name[1])
                actual += 1
    if not saw_section and 'No version information found in this file.' not in text:
        raise ValueError('Unrecognized readelf version-info output')
    if actual != expected or actual_files != expected_files:
        raise ValueError('Incomplete version-needs output')
    required = {family: [] for family in FAMILIES}
    unmodeled = []
    for name in sorted(names):
        family = next((key for key in FAMILIES if name.startswith(key + '_')), None)
        if family:
            value = name[len(family) + 1:]
            numeric(value)  # Private/non-numeric requirements need explicit handling.
            required[family].append(value)
        else:
            unmodeled.append(name)
    return required, unmodeled


def parse_dynamic(text):
    if 'Dynamic section at offset ' not in text and 'There is no dynamic section in this file.' not in text:
        raise ValueError('Unrecognized readelf dynamic output')
    return sorted(set(re.findall(r'\(NEEDED\).*Shared library: \[([^\]]+)\]', text)))


def readelf(tool, option, path):
    return subprocess.check_output([tool, option, '--wide', '--', str(path)],
                                   env={**os.environ, 'LC_ALL': 'C', 'LANG': 'C'},
                                   text=True, stderr=subprocess.PIPE)


def scan(stage, profile, manifest=MANIFEST, tool='readelf'):
    contract = profile_contract(profile, manifest)
    if contract['osFamily'] != 'Linux':
        raise ValueError('ELF inspection requires a Linux profile')
    ceilings = contract['maxRequiredSymbolVersions']
    for family in FAMILIES:
        if ceilings[family] is not None:
            numeric(ceilings[family])
    root = stage.resolve(strict=True)
    if not root.is_dir():
        raise ValueError('Stage must be a directory')
    artifacts = []
    errors = []
    maxima = {family: None for family in FAMILIES}
    for path in sorted(root.rglob('*')):
        if path.is_symlink() and (not path.resolve().is_relative_to(root) or path.is_dir()):
            raise ValueError('Stage symlink escapes or points to a directory')
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError('Stage contains a special or dangling file')
        with path.open('rb') as stream:
            header = stream.read(64)
        if not header.startswith(b'\x7fELF'):
            continue
        entry = {'path': path.relative_to(root).as_posix(),
                 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        artifacts.append(entry)
        try:
            machine = {'x86_64': 62, 'arm64': 183}[contract['architecture']]
            if (len(header) < 64 or header[4:7] != b'\x02\x01\x01'
                    or int.from_bytes(header[16:18], 'little') not in (2, 3)
                    or int.from_bytes(header[18:20], 'little') != machine):
                raise ValueError('ELF class/endianness/type/architecture differs from profile')
            versions = readelf(tool, '--version-info', path)
            dynamic = readelf(tool, '--dynamic', path)
            entry['rawVersionInfo'] = versions
            entry['rawDynamic'] = dynamic
            entry['requiredSymbolVersions'], entry['unmodeledVersionRequirements'] = parse_needs(versions)
            entry['neededLibraries'] = parse_dynamic(dynamic)
            for family, values in entry['requiredSymbolVersions'].items():
                if values:
                    high = max(values, key=numeric)
                    if maxima[family] is None or numeric(high) > numeric(maxima[family]):
                        maxima[family] = high
        except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as exc:
            entry['error'] = str(exc)
            if isinstance(exc, subprocess.CalledProcessError):
                entry['stderr'] = exc.stderr
            errors.append(entry['path'] + ': ' + str(exc))
    if not artifacts:
        errors.append('No shipped ELF artifacts found')
    pending = []
    for family in FAMILIES:
        ceiling, required = ceilings[family], maxima[family]
        if ceiling is None:
            pending.append(f'{family}: baseline runtime ceiling has not been measured')
        elif required is not None and numeric(required) > numeric(ceiling):
            errors.append(f'{family}_{required} exceeds declared baseline {ceiling}')
    return {'schemaVersion': 1, 'profile': profile, 'supportContract': contract,
            'status': 'fail' if errors else ('pending-baseline' if pending else 'pass'),
            'releaseQualified': False, 'requiredMaxima': maxima, 'errors': errors,
            'pending': pending, 'artifacts': artifacts,
            'scope': 'All shipped ELF files; GLIBC/GLIBCXX/CXXABI requirements only. '
                     'System dependency closure, other symbol families, ISA, DM ABI and runtime execution require qualification.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', required=True, type=Path)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--manifest', type=Path, default=MANIFEST)
    parser.add_argument('--readelf', default='readelf')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    report = scan(args.stage, args.profile, args.manifest, args.readelf)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    raise SystemExit({'pass': 0, 'fail': 1, 'pending-baseline': 2}[report['status']])
