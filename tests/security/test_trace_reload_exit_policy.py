"""Run the compiled, isolated GoogleTest main through real bounded CTest cases.

This checks producer exit classification, not driver/unload feature behavior.
The control executable is compiled from the real reload source with only its
synthetic assertion arm enabled. No Python copy of the C++ policy is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET


CASES = {
    'pass': ('Pass', 0),
    'all_skip': ('SkipA:TraceReloadExitPolicyControl.SkipB', 77),
    'failure_and_skip': ('Failure:TraceReloadExitPolicyControl.SkipA', 1),
    'failure_then_skip': ('FailureThenSkip', 1),
    'ad_hoc_and_skip': ('AdHocFailureAndSkip', 1),
    'partial_skip': ('Pass:TraceReloadExitPolicyControl.SkipA', 1),
    'zero_selected': ('NoSuchSelectedCase', 1),
    'forged_skip_text': ('SkipTextFailure', 1),
}


def cmake_argument(value):
    if ']==]' in value:
        raise RuntimeError('Unsupported control path delimiter')
    return '[==[' + value + ']==]'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--ctest', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    binary, ctest = args.binary.resolve(strict=True), args.ctest.resolve(strict=True)
    work = Path(tempfile.mkdtemp(prefix='trace-reload-policy-', dir=args.work_root))
    # Retain even unexpected failures for diagnosis, rather than deleting logs.
    print('CONTROL_EVIDENCE_DIR=' + str(work), flush=True)
    env = {key: value for key, value in os.environ.items() if not key.startswith('GTEST_')}
    declarations = []
    for name, (selected, _) in CASES.items():
        command = [str(binary), '--gtest_filter=TraceReloadExitPolicyControl.' + selected,
                   '--gtest_output=xml:' + (work / (name + '.xml')).as_posix()]
        declarations.append('add_test(' + name + ' ' + ' '.join(map(cmake_argument, command)) + ')')
        declarations.append(f'set_tests_properties({name} PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77)')
    (work / 'CTestTestfile.cmake').write_text('\n'.join(declarations) + '\n', encoding='utf-8')
    completed = subprocess.run([str(ctest), '--test-dir', str(work), '-j1', '--output-on-failure',
                                '--output-junit', str(work / 'ctest.xml'),
                                '--output-log', str(work / 'ctest.log')],
                               env=env, capture_output=True, text=True, timeout=85)
    (work / 'stdout.txt').write_text(completed.stdout, encoding='utf-8')
    (work / 'stderr.txt').write_text(completed.stderr, encoding='utf-8')
    if completed.returncode != 8:
        raise RuntimeError('Expected actual failing CTest controls, got ' + str(completed.returncode))
    root = ET.parse(work / 'ctest.xml').getroot()
    cases = list(root.iter('testcase'))
    if len(cases) != len(CASES) or {case.get('name') for case in cases} != set(CASES):
        raise RuntimeError('Missing or duplicate actual CTest controls')
    for case in cases:
        name = case.get('name')
        expected = CASES[name][1]
        output = case.findtext('system-out') or ''
        lines = [line for line in output.splitlines() if line.startswith('ODBCPP_TRACE_RELOAD_RESULT_V1 ')]
        if len(lines) != 1 or not lines[0].endswith(' exit=' + str(expected)):
            raise RuntimeError('Wrong actual native policy result: ' + name)
        if expected == 77:
            skip = case.find('skipped')
            if (skip is None or skip.get('message') != 'SKIP_RETURN_CODE=77'
                    or case.find('failure') is not None):
                raise RuntimeError('All-skip policy did not become an actual return-code skip')
        elif expected == 0:
            if case.get('status') != 'run' or any(case.find(tag) is not None for tag in ('failure', 'error', 'skipped')):
                raise RuntimeError('Passing control did not pass')
        elif case.find('failure') is None or case.find('skipped') is not None:
            raise RuntimeError('A failure was masked as skip/pass: ' + name)
    # Listing is an intentionally non-executing inventory interface, not zero
    # selected runtime success. It must not emit an execution acceptance marker.
    listed = subprocess.run([str(binary), '--gtest_list_tests'], cwd=work, env=env,
                            capture_output=True, text=True, timeout=5)
    if (listed.returncode != 0 or 'TraceReloadExitPolicyControl.' not in listed.stdout
            or 'ODBCPP_TRACE_RELOAD_RESULT_V1 ' in listed.stdout):
        raise RuntimeError('Native inventory-only interface was broken')
    selected = [str(binary), '--gtest_filter=TraceReloadExitPolicyControl.Pass']
    default = work / 'odbcpp-trace-reload-gtest.xml'
    fallback = subprocess.run(selected, cwd=work, env=env, capture_output=True, text=True, timeout=5)
    if fallback.returncode != 0 or ET.parse(default).getroot().get('tests') != '1':
        raise RuntimeError('The empty-output default did not write standard native XML')
    previous_default = default.read_bytes()
    alternate = work / 'external-output.xml'
    external_env = dict(env, GTEST_OUTPUT='xml:' + alternate.as_posix())
    external = subprocess.run(selected, cwd=work, env=external_env,
                              capture_output=True, text=True, timeout=5)
    if (external.returncode != 0 or ET.parse(alternate).getroot().get('tests') != '1'
            or default.read_bytes() != previous_default):
        raise RuntimeError('Existing GTEST_OUTPUT custody was overridden')
    result = {'scope': 'actual synthetic native/CTest exit policy only; no driver loaded',
              'ctestExit': completed.returncode, 'cases': len(cases), 'passedControls': len(cases),
              'inventoryOnlyExit': listed.returncode, 'defaultOutputExit': fallback.returncode,
              'externalOutputExit': external.returncode,
              'binary': str(binary), 'binarySha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'ctest': str(ctest), 'ctestSha256': hashlib.sha256(ctest.read_bytes()).hexdigest()}
    (work / 'policy-results.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
