#!/usr/bin/env python3
"""Validate complete live registration without reading profile inputs or executing images."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def preflight(index_path, expected_path, output, cmake='cmake'):
    """Use the real registrar with empty synthetic environments; no CTest execution."""
    index_path = index_path.resolve()
    expected_path = expected_path.resolve()
    output = output.resolve()
    index = json.loads(index_path.read_text(encoding='utf-8-sig'))
    # Retain actual image hashes and ordered identities; never open or copy
    # credential-bearing environment files from the supplied public index.
    index['profiles'] = {name: {'environment': []} for name in index['profiles']}
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    shadow = output / 'public-registration.json'
    shadow.write_text(json.dumps(index), encoding='utf-8')
    source = Path(__file__).resolve().parents[2] / 'tests/development/live_suite'
    command = [cmake, '-S', str(source), '-B', str(output / 'registration'),
               '-DODBCPP_LIVE_INDEX=' + str(shadow),
               '-DODBCPP_LIVE_EXPECTED=' + str(expected_path)]
    # Preserve original bytes: Windows console encodings must not corrupt a
    # Unicode path/diagnostic or prevent publication of a configure failure.
    with (output / 'configure.log').open('xb') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    report = {'schema_version': 1, 'preflight_only': True, 'configure_exit': result.returncode,
              'case_count': len(index['cases']), 'profile_count': len(index['profiles']),
              'index_sha256': hashlib.sha256(index_path.read_bytes()).hexdigest(),
              'expected_sha256': hashlib.sha256(expected_path.read_bytes()).hexdigest(),
              'resource_operations': 0, 'private_inputs_read': False,
              'test_images_executed': False, 'live_admission_granted': False,
              'limits': 'Expected-inventory provenance and applicability still require review; '
                        'this validates registration, not credentials, resources or live execution.'}
    (output / 'preflight.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--index', type=Path, required=True, help='Public image/profile/case index')
    parser.add_argument('--expected', type=Path, required=True, help='Independent complete expected inventory')
    parser.add_argument('--output', type=Path, required=True, help='Fresh owned output directory')
    parser.add_argument('--cmake', default='cmake')
    args = parser.parse_args()
    # Match the registrar's POSIX generated-metadata privacy requirement.
    import os
    os.umask(0o077)
    result = preflight(args.index, args.expected, args.output, args.cmake)
    print(json.dumps(result))
    return result['configure_exit']


if __name__ == '__main__':
    raise SystemExit(main())
