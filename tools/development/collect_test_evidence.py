#!/usr/bin/env python3
"""Record public bindings and summarize standard XML; no execution or admission."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


def binding(path):
    path = Path(path).resolve()
    return {'path': str(path), 'bytes': path.stat().st_size,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def xml_summary(path):
    root = ET.parse(path).getroot()
    rows = list(root.iter('testcase'))
    counts = {'cases': len(rows), 'fail': 0, 'skip': 0, 'disabled': 0}
    for row in rows:
        if row.find('failure') is not None or row.find('error') is not None:
            counts['fail'] += 1
        elif row.find('skipped') is not None or row.get('result') == 'skipped':
            counts['skip'] += 1
        elif row.get('status') == 'notrun' or row.get('result') == 'suppressed':
            counts['disabled'] += 1
    counts['pass'] = counts['cases'] - sum(counts[k] for k in ('fail', 'skip', 'disabled'))
    return counts


def collect(args):
    source = args.source.resolve()
    def git(*command):
        return subprocess.check_output(['git', '-C', str(source), *command],
                                       text=True).strip()
    diff = subprocess.check_output(['git', '-C', str(source), 'diff', 'HEAD', '--binary'])
    report = {'schema_version': 1, 'recorded_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
              'source': {'path': str(source), 'head': git('rev-parse', 'HEAD'),
                         'tree': git('rev-parse', 'HEAD^{tree}'),
                         'status': git('status', '--porcelain'),
                         'tracked_diff_sha256': hashlib.sha256(diff).hexdigest()},
              'images_now': [binding(p) for p in args.image],
              'ctest_exit': args.ctest_exit, 'ctest': None, 'gtest': [],
              'xml_errors': [], 'cleanup_receipt': None,
              'acceptance_claimed': False,
              'limits': 'Current file bindings; compare before/after. Missing XML is not PASS. '
                        'Source untracked contents, applicability, operational cleanup and acceptance require review.'}
    if args.build:
        cache = args.build / 'CMakeCache.txt'
        report['configuration'] = binding(cache)
        report['logs'] = [binding(p) for p in (args.build / 'Testing/Temporary').glob('*.log')]
    if args.inventory:
        inventory = json.loads(args.inventory.read_text(encoding='utf-8-sig'))
        rows = inventory.get('tests', inventory.get('cases'))
        if not isinstance(rows, list):
            raise ValueError('Expected CTest JSON tests or public live cases inventory')
        report['inventory'] = dict(binding(args.inventory), count=len(rows))
    if args.ctest_junit:
        try:
            report['ctest'] = dict(binding(args.ctest_junit), **xml_summary(args.ctest_junit))
        except (OSError, ET.ParseError) as error:
            report['xml_errors'].append({'file': str(args.ctest_junit), 'error': type(error).__name__})
    if args.gtest_dir:
        for path in sorted(args.gtest_dir.glob('*.xml')):
            try:
                report['gtest'].append(dict(binding(path), **xml_summary(path)))
            except (OSError, ET.ParseError) as error:
                report['xml_errors'].append({'file': str(path), 'error': type(error).__name__})
    if args.cleanup_receipt:
        # Hash/custody only: no private contents or inferred cleanup acceptance.
        report['cleanup_receipt'] = binding(args.cleanup_receipt)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, help='Public inventory only; never private live CTest metadata')
    parser.add_argument('--image', type=Path, action='append', default=[])
    parser.add_argument('--ctest-junit', type=Path)
    parser.add_argument('--ctest-exit', type=int)
    parser.add_argument('--gtest-dir', type=Path)
    parser.add_argument('--cleanup-receipt', type=Path)
    args = parser.parse_args()
    if bool(args.ctest_junit) != (args.ctest_exit is not None):
        parser.error('--ctest-junit and actual --ctest-exit are required together')
    report = collect(args)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps({'report_written': str(args.output), 'ctest_exit': args.ctest_exit,
                      'ctest': report['ctest'], 'gtest_xml_files': len(report['gtest']),
                      'xml_errors': len(report['xml_errors']), 'acceptance_claimed': False}))


if __name__ == '__main__':
    main()
