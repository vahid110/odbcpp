#!/usr/bin/env python3
"""Validate the fixed, database-free Driver Manager checkpoint; execute no tests."""
import argparse
from collections import Counter
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

_spec = importlib.util.spec_from_file_location(
    'dm_evidence_collector', Path(__file__).resolve().with_name('collect_test_evidence.py'))
_collector = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_collector)

IDENTITY = 'DriverManagerIdentity'
CASES = {
    'identity': [(IDENTITY, name) for name in (
        'ExactQualifiedBuildAndProfileMatch',
        'SameSqlDmVersionCannotEnrollUnknownOrOtherBuild',
        'SameSqlDmVersionCannotSelectAnotherAbiOrEnvironment',
        'ActualProviderAndNullBufferRouteShareQualification',
        'ByteComparisonRejectsDifferentBuildAndLength')],
    'direct-login-timeout': [('ConnectionAttributeIntegrationTest', 'LoginTimeoutOutputWidth')],
    'manager-login-timeout': [('ConnectionAttributeIntegrationTest', 'LoginTimeoutOutputWidth')],
    'unicode-conversion': [('DriverManagerUnicodeHarnessTest',
                            'OverrideTextDecodesCompleteScalarsIntoManagerUnits')],
}
OWNERS = {
    'identity': ('dm_offline_identity', 'test_dm_identity'),
    'direct-login-timeout': ('dm_offline_direct_login_timeout', 'it_handle_lifecycle'),
    'manager-login-timeout': ('dm_offline_manager_login_timeout', 'dm_it_handle_lifecycle'),
    'unicode-conversion': ('dm_offline_unicode_conversion', 'it_driver_manager_unicode'),
}


def source_binding():
    source = Path(__file__).resolve().parents[2]
    def git(*args):
        return subprocess.check_output(['git', '-C', str(source), *args], stderr=subprocess.PIPE)
    diff = git('diff', 'HEAD', '--binary')
    return {'path': str(source), 'head': git('rev-parse', 'HEAD').decode().strip(),
            'tree': git('rev-parse', 'HEAD^{tree}').decode().strip(),
            'status': git('status', '--porcelain').decode().strip(),
            'tracked_diff_sha256': hashlib.sha256(diff).hexdigest()}


def read_xml(path, expected, *, gtest):
    """Return original case facts even when count/status validation fails."""
    facts = {'path': str(path), 'cases': []}
    errors = []
    try:
        facts.update(_collector.binding(path, max_bytes=_collector.DEFAULT_LIMITS.max_bytes))
        root = _collector.parse_xml_file(path)
        if root.tag not in ('testsuite', 'testsuites'):
            errors.append('unexpected XML root')
        for row in root.iter('testcase'):
            facts['cases'].append({'attributes': dict(row.attrib),
                                   'properties': [dict(p.attrib) for p in row.findall('./properties/property')]})
            if gtest and (row.get('status') != 'run' or row.get('result') != 'completed'):
                errors.append('GoogleTest case was not run/completed')
            if not gtest and (row.get('status') not in (None, 'run') or
                              row.get('result') not in (None, 'completed')):
                errors.append('CTest case was not run/completed')
        actual = [(r['attributes'].get('classname'), r['attributes'].get('name'))
                  if gtest else r['attributes'].get('name') for r in facts['cases']]
        if Counter(actual) != Counter(expected):
            errors.append('case inventory mismatch (missing, duplicate, empty or substituted case)')
        if any(node.tag in ('failure', 'error', 'skipped') for node in root.iter()):
            errors.append('XML contains failure/error/skipped')
        for suite in root.iter():
            if suite.tag not in ('testsuite', 'testsuites'):
                continue
            count = sum(1 for _ in suite.iter('testcase'))
            if suite.get('tests') != str(count):
                errors.append('suite tests count disagrees with actual cases')
            for name in ('failures', 'errors', 'disabled', 'skipped'):
                if suite.get(name, '0') != '0':
                    errors.append('nonzero or malformed suite ' + name)
    except (OSError, ET.ParseError, ValueError) as error:
        errors.append('cannot read XML: ' + type(error).__name__)
    return facts, errors


def provider_binding(facts):
    rows = [row for row in facts['cases'] if row['attributes'].get('name') ==
            'ActualProviderAndNullBufferRouteShareQualification']
    if len(rows) != 1:
        raise ValueError('actual-provider case missing or duplicated')
    properties = rows[0]['properties']
    names = ('dm_version', 'dm_loaded_library', 'dm_pointer_bytes',
             'dm_application_sqlwchar_bytes', 'dm_configured_driver_sqlwchar_bytes',
             'dm_odbc_environment', 'dm_configuration_provenance')
    values = {}
    for name in names:
        matches = [p.get('value') for p in properties if p.get('name') == name]
        if len(matches) != 1 or not matches[0]:
            raise ValueError('missing/duplicate/empty provider property: ' + name)
        values[name] = matches[0]
    if (values['dm_pointer_bytes'] not in ('4', '8') or
            values['dm_application_sqlwchar_bytes'] not in ('2', '4') or
            values['dm_configured_driver_sqlwchar_bytes'] not in ('2', '4') or
            values['dm_odbc_environment'] != '3'):
        raise ValueError('invalid observed/configured ABI or ODBC profile')
    if not re.fullmatch(
            r'default_lookup=(Windows_ODBC_registry|manager_defined_ini_locations);'
            r'ODBCINI=(set|unset);ODBCSYSINI=(set|unset);ODBCINSTINI=(set|unset)',
            values['dm_configuration_provenance']):
        raise ValueError('invalid configuration provenance')
    provider = Path(values['dm_loaded_library'])
    if not provider.is_absolute() or not provider.is_file():
        raise ValueError('observed provider is not an absolute regular file')
    result = _collector.binding(provider)
    if result['bytes'] == 0:
        raise ValueError('observed provider file is empty')
    return result


def validate(build, exit_file, config=None):
    build, exit_file = Path(build).resolve(), Path(exit_file).resolve()
    report = {'schema_version': 1, 'recorded_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
              'build': str(build), 'config': config, 'source': None, 'configuration': None,
              'images_now': {}, 'ctest_exit': None, 'ctest_exit_file': None,
              'ctest': None, 'gtest': {}, 'errors': [], 'passed': False,
              'limits': 'Current source/configuration/image/XML bindings, not historical execution identity. '
                        'The caller must freeze source and images and use fresh XML for this run. '
                        'Untracked source contents and live/platform acceptance are not certified.'}
    errors = report['errors']
    try:
        report['source'] = source_binding()
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        errors.append('source binding failed: ' + type(error).__name__)
    cache = build / 'CMakeCache.txt'
    try:
        report['configuration'] = _collector.binding(cache)
    except OSError as error:
        errors.append('configuration binding failed: ' + type(error).__name__)
    try:
        report['ctest_exit_file'] = _collector.binding(exit_file, max_bytes=64)
        raw = exit_file.read_text(encoding='utf-8-sig').strip()
        if not re.fullmatch(r'-?\d+', raw):
            raise ValueError('not an integer')
        report['ctest_exit'] = int(raw)
        if report['ctest_exit'] != 0:
            errors.append('actual CTest exit is not zero')
    except (OSError, ValueError, UnicodeError, ET.ParseError) as error:
        errors.append('actual CTest exit unavailable: ' + type(error).__name__)
    image_dir = build / 'tests'
    if config:
        if Path(config).name != config or config in ('.', '..') or '/' in config or '\\' in config:
            errors.append('config must be a single directory name')
            image_dir = None
        else:
            image_dir /= config
    paths = {}
    for key, (_, image) in OWNERS.items():
        if image_dir is None:
            continue
        candidates = [p for p in (image_dir / image, image_dir / (image + '.exe')) if p.is_file()]
        if len(candidates) != 1:
            errors.append(key + ': missing or ambiguous owning image')
            continue
        paths[key] = candidates[0]
        try:
            report['images_now'][key] = _collector.binding(candidates[0])
            if report['images_now'][key]['bytes'] == 0:
                errors.append(key + ': empty owning image')
        except OSError as error:
            errors.append(key + ': image binding failed: ' + type(error).__name__)
    directory = build / 'dm-offline'
    documents = [('ctest', directory / 'ctest.xml', [v[0] for v in OWNERS.values()])]
    documents.extend((key, directory / (key + '.xml'), expected) for key, expected in CASES.items())
    for key, path, expected in documents:
        facts, problems = read_xml(path, expected, gtest=key != 'ctest')
        if key == 'ctest':
            report['ctest'] = facts
        else:
            report['gtest'][key] = facts
        errors.extend(key + ': ' + problem for problem in problems)
        # Two seconds tolerates coarse filesystem timestamps. This catches obvious
        # stale artifacts; it deliberately does not claim a run-authentication scheme.
        try:
            owners = list(paths.values()) if key == 'ctest' else [paths[key]]
            if path.stat().st_mtime + 2 < max(p.stat().st_mtime for p in [cache, *owners]):
                errors.append(key + ': XML predates build inputs/image')
            if path.stat().st_mtime > exit_file.stat().st_mtime + 2:
                errors.append(key + ': XML postdates actual exit receipt')
        except (OSError, KeyError):
            pass  # Missing inputs have explicit errors above.
    try:
        report['observed_provider_now'] = provider_binding(report['gtest']['identity'])
    except (OSError, ValueError) as error:
        errors.append('provider binding failed: ' + str(error))
    report['passed'] = not errors
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--ctest-exit-file', type=Path, required=True)
    parser.add_argument('--config')
    args = parser.parse_args()
    report = validate(args.build, args.ctest_exit_file, args.config)
    output = args.build.resolve() / 'dm-offline/evidence.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    # Never replace a previous evidence report. The caller archives prior runs.
    with output.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps({'report': str(output), 'passed': report['passed'], 'errors': report['errors']}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
