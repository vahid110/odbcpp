#!/usr/bin/env python3
"""Finite Linux pilot evidence; orchestration only, never product assertions.

GNU time maximum child RSS is NOT the summed concurrent process-tree peak.
The job deadline and fixed phase timeouts bound one manually selected comparison.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import time
import xml.etree.ElementTree as ET

SUITES = (
    'test_redshift_catalog_execution', 'test_backend_contract',
    'test_numeric_scale_conversion', 'test_redshift_data_conversion',
)
CHECKS = (
    'test_architecture_boundaries', 'test_sdk_contract_boundary',
    'test_sdk_contract_boundary_failures', 'test_internal_source_partitions',
    'test_internal_include_closure', 'test_crypto_manifest',
)
SELECTED = SUITES + CHECKS
REGEX = '^(' + '|'.join(SELECTED) + ')$'
SEED_HEAD = 'cac4e780dcc9ce84fcc8a3d104ab1afbf4ba0547'
CHANGED_HEAD = 'd5f189e9e8e7184c0d601a7c5a89ee244bc93e70'
REAL_ROLES = ('seed-a', 'cold-b', 'cached-b')
ADDED_CASES = (
    'BackendContractTest.FloatingBinaryResultRowWiseOffsetsPreflightCapacityAndRepair',
    'BackendContractTest.FloatingBinaryResultColumnWiseStrideUnalignedSplitNullAndSpan',
    'NumericScaleApiTest.FloatingBinaryResultLiteralWidthsRoundingSpecialsRetryAndNull',
    'NumericScaleApiTest.FloatingBinaryResultBoundIndicatorOnlyValueValidationAndRecovery',
    'NumericScaleApiTest.FloatingBinaryResultGetDataErrorsLeaveRetryAndFollowingRowUsable',
    'NumericScaleApiTest.FloatingBinaryResultArdNullPrecedencePreparedOwnershipAndRefill',
    'ResultTypesTest.FloatingBinaryResultPairsKeepNativeDefaultsAndOtherRefusals',
)
BUILD = Path('build-ubuntu-pilot')
EVIDENCE = Path('ubuntu-pilot-evidence')
PROFILE = (
    '-DBUILD_EXAMPLES=OFF', '-DBUILD_TESTING=ON', '-DTARGET_DATABASE=REDSHIFT',
    '-DODBCPP_CRYPTO_PROVIDER=OPENSSL', '-DODBCPP_CRYPTO_LINKAGE=SYSTEM_SHARED',
    '-DODBC_DRIVER_MANAGER_FLAVOR=UNIXODBC', '-DODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=2',
    '-DODBCPP_EXPECT_DM_SQLWCHAR_SIZE=2', '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache',
    '-DCMAKE_C_COMPILER_LAUNCHER=ccache',
)


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def write(name, value):
    EVIDENCE.mkdir(exist_ok=True)
    (EVIDENCE / name).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def output(*cmd):
    return subprocess.check_output(cmd, text=True, timeout=60).strip()


def source(expected, directory=None):
    git = ['git'] if directory is None else ['git', '-C', str(directory)]
    head = output(*git, 'rev-parse', 'HEAD')
    if not re.fullmatch('[0-9a-f]{40}', expected) or head != expected:
        raise ValueError('selected-ref HEAD differs from reviewed immutable HEAD')
    # Untracked owned build/evidence outputs are expected; tracked inputs stay fixed.
    if output(*git, 'status', '--porcelain', '--untracked-files=no'):
        raise ValueError('tracked source changed')
    return {'head': head, 'tree': output(*git, 'rev-parse', 'HEAD^{tree}')}



def real_product_head(role, seed, changed):
    if role not in REAL_ROLES or (seed, changed) != (SEED_HEAD, CHANGED_HEAD):
        raise ValueError('real-change experiment requires the exact reviewed A/B pair and role')
    return seed if role == 'seed-a' else changed


def empty_cache():
    path = Path(os.environ['CCACHE_DIR'])
    if path.exists():
        raise ValueError('fresh runner must start without a compiler cache')
    path.mkdir(parents=True)


def real_cache_key(seed, workflow, compatibility, run, attempt):
    if seed != SEED_HEAD or not re.fullmatch('[0-9a-f]{40}', workflow) or not re.fullmatch('[0-9a-f]{64}', compatibility):
        raise ValueError('invalid exact seed/workflow/compatibility cache identity')
    if not run.isdigit() or not attempt.isdigit():
        raise ValueError('invalid run/attempt cache identity')
    return f'ubuntu-real-change-{seed}-{workflow}-{run}-{attempt}-{compatibility}'


def compatibility_digest(dependencies, commands, paths):
    # Product contents/HEAD deliberately excluded; ccache validates changed inputs.
    return digest({'dependencies': dependencies, 'profile': PROFILE,
                   'compile_commands': commands, 'paths': paths,
                   'cache_compiler_check': os.environ.get('CCACHE_COMPILERCHECK', '')})


def check_seed_cache():
    local = json.loads((EVIDENCE / 'cache-key.json').read_text())
    expected = real_cache_key(SEED_HEAD, os.environ['PILOT_WORKFLOW_HEAD'],
                              local['compatibility_digest'], os.environ['GITHUB_RUN_ID'],
                              os.environ['GITHUB_RUN_ATTEMPT'])
    if os.environ['PILOT_SEED_COMPATIBILITY'] != local['compatibility_digest'] or os.environ['PILOT_SEED_CACHE_KEY'] != expected:
        raise ValueError('selected A cache key or strict compiler/dependency/flags/profile/path compatibility differs')
    write('selected-seed-cache.json', {'key': expected, 'seed_head': SEED_HEAD,
          'workflow_head': os.environ['PILOT_WORKFLOW_HEAD'],
          'compatibility_digest': local['compatibility_digest']})



def cache_comparison(role, restored_hit, compiler_hits):
    if role in ('warm', 'cached-b') and restored_hit != 'true':
        return 'inconclusive: exact warm cache miss'
    if role == 'cached-b' and not compiler_hits:
        return 'inconclusive: cache restored but zero useful compiler hits'
    return 'evidence collected; independent comparison pending'


def inventory_delta(seed, changed, cold_digest=None):
    if seed['ctests'] != changed['ctests']:
        raise ValueError('seed and candidate CTest selections differ')
    flatten = lambda x: {(suite, name) for suite, names in x['google_tests'].items() for name in names}
    before, after = flatten(seed), flatten(changed)
    delta = {'added': sorted(after - before), 'removed': sorted(before - after)}
    expected = {('test_backend_contract' if name.startswith('BackendContractTest.') else
                 'test_numeric_scale_conversion' if name.startswith('NumericScaleApiTest.') else
                 'test_redshift_data_conversion', name) for name in ADDED_CASES}
    write('seed-candidate-inventory-delta.json', delta)
    if delta['removed'] or set(delta['added']) != expected:
        raise ValueError('actual inventory delta differs from the seven reviewed FLOAT cases; differences retained')
    if cold_digest is not None and digest(changed) != cold_digest:
        raise ValueError('cached B inventory differs from actual cold B')
    return delta


def measure(name, command, timeout):
    """Retain exit and timing even for genuine failures; kill owned group on timeout."""
    EVIDENCE.mkdir(exist_ok=True)
    started = utc()
    tick = time.monotonic()
    rss_path = EVIDENCE / (name + '-time.txt')
    with (EVIDENCE / (name + '.log')).open('w') as log:
        proc = subprocess.Popen(
            ['/usr/bin/time', '-v', '-o', str(rss_path), *command],
            stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        timed_out = False
        try:
            code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait(timeout=15)
            code = 124
    timing = rss_path.read_text() if rss_path.exists() else ''
    match = re.search(r'Maximum resident set size \(kbytes\):\s*(\d+)', timing)
    record = {
        'command': command, 'started_at': started, 'finished_at': utc(),
        'elapsed_seconds': time.monotonic() - tick, 'exit': code,
        'timed_out': timed_out, 'gnu_time_max_child_rss_kib': int(match[1]) if match else None,
        'rss_limit': 'maximum child RSS, not summed concurrent process-tree peak',
        'disk_free_bytes_after': shutil.disk_usage('.').free,
    }
    write(name + '.json', record)
    if code:
        print((EVIDENCE / (name + '.log')).read_text(), flush=True)
        raise RuntimeError(f'{name} failed with exit {code}; original log retained')
    return record


def inventory_names(inventory):
    names = [test['name'] for test in inventory['tests']]
    if len(names) != len(SELECTED) or set(names) != set(SELECTED):
        raise ValueError(f'expected ten selected CTests, got {names}')
    return names


def gtest_names(text):
    names, suite = [], ''
    for line in text.splitlines():
        bare = line.split('#', 1)[0].rstrip()
        if not bare or bare.startswith('Running main()'):
            continue
        if bare.startswith('  ') and suite:
            names.append(suite + bare.strip())
        elif bare.endswith('.') and not bare.startswith(' '):
            suite = bare
    if not names or len(set(names)) != len(names) or any('DISABLED_' in n for n in names):
        raise ValueError('empty, duplicate or disabled GoogleTest inventory')
    return sorted(names)


def xml_result(path, expected):
    root = ET.parse(path).getroot()
    cases = list(root.iter('testcase'))
    names = [c.attrib['classname'] + '.' + c.attrib['name'] for c in cases]
    failures = len(list(root.iter('failure')))
    errors = len(list(root.iter('error')))
    skips = len(list(root.iter('skipped')))
    disabled = sum(c.get('status') == 'notrun' or c.get('result') in ('skipped', 'suppressed') for c in cases)
    result = {'count': len(cases), 'failures': failures, 'errors': errors,
              'skips': skips, 'disabled_or_notrun': disabled, 'names': sorted(names)}
    for attribute, actual in (('tests', len(cases)), ('failures', failures), ('errors', errors), ('disabled', disabled)):
        if attribute in root.attrib and int(root.attrib[attribute]) != actual:
            raise ValueError(f'XML declared count mismatch: {attribute}')
    if sorted(names) != sorted(expected) or len(set(names)) != len(names) or failures or errors or skips or disabled:
        raise ValueError(f'inventory/result mismatch or unsuccessful cases: {path}: {result}')
    return result


def init(expected, submitted_at, mode):
    identity = source(expected)
    if mode in REAL_ROLES:
        required = real_product_head(mode, os.environ['PILOT_SEED_HEAD'], os.environ['PILOT_CHANGED_HEAD'])
        if expected != required:
            raise ValueError('product checkout differs from selected A/B role')
        workflow_root = Path(os.environ['PILOT_WORKFLOW_ROOT'])
        workflow = source(os.environ['PILOT_WORKFLOW_HEAD'], workflow_root)
        if workflow['head'] != os.environ['GITHUB_SHA']:
            raise ValueError('workflow checkout differs from selected dispatch workflow HEAD')
        workflow['files'] = {name: sha(workflow_root / name) for name in (
            '.github/workflows/ci.yml', '.github/workflows/ubuntu-development-pilot.yml',
            'tools/ci/ubuntu_development_pilot.py', 'tools/ci/test_ubuntu_development_pilot.py')}
        identity['workflow_source'] = workflow
        identity['product_path'] = str(Path.cwd())
    elif mode not in ('cold', 'warm'):
        raise ValueError('unknown pilot role')
    submitted = datetime.fromisoformat(submitted_at.replace('Z', '+00:00'))
    if submitted.tzinfo is None or submitted > datetime.now(timezone.utc):
        raise ValueError('submission must be a UTC timestamp preceding this runner')
    identity.update({
        'mode': mode, 'submitted_at': submitted.isoformat(), 'runner_evidence_started_at': utc(),
        'submission_to_runner_evidence_seconds': (datetime.now(timezone.utc) - submitted).total_seconds(),
        'profile': list(PROFILE), 'selected': list(SELECTED),
        'runner': {k: os.environ.get(k) for k in (
            'RUNNER_OS', 'RUNNER_ARCH', 'RUNNER_NAME', 'ImageOS', 'ImageVersion',
            'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_SHA', 'GITHUB_REF',
            'GITHUB_WORKFLOW_REF', 'GITHUB_WORKFLOW_SHA',
        )},
        'machine': platform.machine(), 'cpu': output('lscpu'),
        'ram': Path('/proc/meminfo').read_text(), 'disk': output('df', '-h', '.'),
        'qualification': 'experimental Linux focused feedback only; full/platform/native/live/final gates pending',
    })
    if platform.machine() != 'x86_64' or os.environ.get('RUNNER_OS') != 'Linux':
        raise ValueError('pilot requires hosted Ubuntu x64')
    write('identity.json', identity)


def configure():
    if BUILD.exists():
        raise ValueError('independent runner must configure a fresh build directory')
    measure('configure', ['cmake', '-S', '.', '-B', str(BUILD), *PROFILE], 300)
    for name in ('CMakeCache.txt', 'compile_commands.json', 'odbcpp-crypto-manifest.json'):
        shutil.copyfile(BUILD / name, EVIDENCE / name)
    deps = {
        'system_packages': output('dpkg-query', '-W', '-f=${Package}=${Version}\n',
                                  'cmake', 'g++', 'gcc', 'libssl-dev', 'unixodbc-dev', 'ccache', 'libc6-dev'),
        'tools': {cmd: output(cmd, '--version') for cmd in ('cmake', 'ctest', 'g++', 'gcc', 'ccache')},
        'compiler_hashes': {cmd: sha(shutil.which(cmd)) for cmd in ('g++', 'gcc')},
        'fetched_source_trees': {},
        'runner_image': {k: os.environ.get(k) for k in ('ImageOS', 'ImageVersion', 'RUNNER_OS', 'RUNNER_ARCH')},
    }
    for dependency in ('googletest', 'spdlog'):
        root = BUILD / '_deps' / (dependency + '-src')
        files = {str(p.relative_to(root)): sha(p) for p in sorted(root.rglob('*')) if p.is_file()}
        if not files:
            raise ValueError(f'missing actual dependency source {dependency}')
        deps['fetched_source_trees'][dependency] = {'files': len(files), 'sha256': digest(files)}
    identity = json.loads((EVIDENCE / 'identity.json').read_text())
    profile_digest = digest({'dependencies': deps, 'profile': PROFILE, 'head': identity['head'], 'tree': identity['tree']})
    key = f"ubuntu-pilot-{identity['head']}-{os.environ['GITHUB_RUN_ID']}-{os.environ['GITHUB_RUN_ATTEMPT']}-{profile_digest}"
    cache_identity = {'key': key, 'profile_digest': profile_digest}
    if identity['mode'] in REAL_ROLES:
        deps['ccache_config'] = output('ccache', '--show-config')
        commands = json.loads((BUILD / 'compile_commands.json').read_text())
        paths = {'product': str(Path.cwd()), 'build': str(BUILD.resolve()),
                 'workflow': str(Path(os.environ['PILOT_WORKFLOW_ROOT']).resolve())}
        compatibility = compatibility_digest(deps, commands, paths)
        key = real_cache_key(SEED_HEAD, identity['workflow_source']['head'], compatibility,
                             os.environ['GITHUB_RUN_ID'], os.environ['GITHUB_RUN_ATTEMPT'])
        cache_identity.update({'key': key, 'compatibility_digest': compatibility,
                               'product_head': identity['head'], 'workflow_head': identity['workflow_source']['head'],
                               'seed_head': SEED_HEAD, 'paths': paths})
    write('dependencies.json', deps)
    write('cache-key.json', cache_identity)
    with open(os.environ['GITHUB_OUTPUT'], 'a') as f:
        f.write(f'cache_key={key}\nprofile_digest={profile_digest}\n')
        if 'compatibility_digest' in cache_identity:
            f.write(f"compatibility_digest={cache_identity['compatibility_digest']}\n")


def inventory():
    listing = json.loads(output('ctest', '--test-dir', str(BUILD), '-R', REGEX, '--show-only=json-v1'))
    inventory_names(listing)
    write('ctest-inventory.json', listing)
    cases = {}
    for suite in SUITES:
        binary = BUILD / 'tests' / suite
        text = output(str(binary), '--gtest_list_tests')
        (EVIDENCE / (suite + '-list.txt')).write_text(text + '\n')
        cases[suite] = {'sha256': sha(binary), 'names': gtest_names(text), 'runtime_dependencies': output('ldd', str(binary))}
    headers = Path('sdk/contract_headers.txt').read_text().splitlines()
    headers = [h.strip() for h in headers if h.strip() and not h.startswith('#')]
    staged = BUILD / 'sdk/contract-include'
    stage_hashes = {h: sha(staged / h) for h in headers}
    if any(stage_hashes[h] != sha(Path('sdk/include') / h) for h in headers):
        raise ValueError('staged SDK headers differ from exact source')
    objects = list((BUILD / 'sdk/CMakeFiles/odbcpp_sdk_header_isolation.dir').rglob('*.o'))
    if len(objects) != len(headers):
        raise ValueError('missing first-only-include compiled objects')
    write('sdk-header-isolation.json', {'headers': stage_hashes, 'objects': {str(p): sha(p) for p in objects}})
    write('compiled-objects.json', {str(p.relative_to(BUILD)): {'bytes': p.stat().st_size, 'sha256': sha(p)} for p in sorted(BUILD.rglob('*.o'))})
    # Compare semantic inventory, not absolute executable paths or compilation hashes.
    semantic = {'ctests': sorted(SELECTED), 'google_tests': {k: v['names'] for k, v in cases.items()}}
    write('executables.json', cases)
    write('semantic-inventory.json', semantic)
    role = os.environ.get('PILOT_MODE')
    binary_digest = digest({suite: entry['sha256'] for suite, entry in cases.items()})
    if role == 'cached-b' and binary_digest != os.environ['PILOT_COLD_BINARY_DIGEST']:
        raise ValueError('independently rebuilt cached B executable hashes differ from cold B')
    if role in ('cold-b', 'cached-b'):
        inventory_delta(json.loads(os.environ['PILOT_SEED_INVENTORY']), semantic,
                        os.environ['PILOT_COLD_INVENTORY_DIGEST'] if role == 'cached-b' else None)
    with open(os.environ['GITHUB_OUTPUT'], 'a') as f:
        f.write(f'inventory_digest={digest(semantic)}\n')
        if role in REAL_ROLES:
            f.write('inventory_json=' + json.dumps(semantic, separators=(',', ':')) + '\n')
            f.write(f'binary_digest={binary_digest}\n')


def test():
    cases = json.loads((EVIDENCE / 'executables.json').read_text())
    env = os.environ.copy()
    # CTest actually runs the owning GoogleTests; retain per-suite native GTest XML.
    os.environ['GTEST_OUTPUT'] = 'xml:' + str(EVIDENCE.resolve()) + '/gtest/'
    try:
        measure('tests', ['ctest', '--test-dir', str(BUILD), '-R', REGEX,
                         '--parallel', '2', '--timeout', '90', '--no-tests=error',
                         '--output-on-failure', '--output-junit', 'pilot-ctest.xml'], 600)
    finally:
        if 'GTEST_OUTPUT' in env:
            os.environ['GTEST_OUTPUT'] = env['GTEST_OUTPUT']
        else:
            os.environ.pop('GTEST_OUTPUT', None)
        if (BUILD / 'pilot-ctest.xml').exists():
            shutil.copyfile(BUILD / 'pilot-ctest.xml', EVIDENCE / 'ctest.xml')
    results = {}
    for suite in SUITES:
        binary = BUILD / 'tests' / suite
        if sha(binary) != cases[suite]['sha256']:
            raise ValueError('executable changed during tests')
        results[suite] = xml_result(EVIDENCE / 'gtest' / (suite + '.xml'), cases[suite]['names'])
    # CTest JUnit uses test name only, so inspect independently of GTest case names.
    root = ET.parse(EVIDENCE / 'ctest.xml').getroot()
    ctests = [c.attrib['name'] for c in root.iter('testcase')]
    if sorted(ctests) != sorted(SELECTED) or any(list(root.iter(tag)) for tag in ('failure', 'error', 'skipped')):
        raise ValueError('ten actual successful CTest results required')
    write('results.json', results)


def finish():
    identity = json.loads((EVIDENCE / 'identity.json').read_text())
    source(identity['head'])
    if 'workflow_source' in identity:
        source(identity['workflow_source']['head'], Path(os.environ['PILOT_WORKFLOW_ROOT']))
    (EVIDENCE / 'ccache-statistics.txt').write_text(output('ccache', '--show-stats', '--verbose') + '\n')
    counters = output('ccache', '--print-stats')
    (EVIDENCE / 'ccache-statistics.tsv').write_text(counters + '\n')
    statistics = {line.split()[0]: int(line.split()[1]) for line in counters.splitlines()}
    write('ccache-statistics.json', statistics)
    subprocess.run(['ccache', '--cleanup'], check=True, timeout=60)
    cache_size = sum(p.stat().st_size for p in Path(os.environ['CCACHE_DIR']).rglob('*') if p.is_file())
    if cache_size > 280 * 1024 * 1024:
        raise ValueError('compiler cache exceeded bounded size')
    hit = os.environ.get('PILOT_CACHE_HIT', '')
    restore = None
    if (EVIDENCE / 'restore-start.txt').exists() and (EVIDENCE / 'restore-end.txt').exists():
        def stamp(name):
            return datetime.fromisoformat((EVIDENCE / name).read_text().strip().replace('Z', '+00:00'))
        restore = (stamp('restore-end.txt') - stamp('restore-start.txt')).total_seconds()
    if identity['mode'] == 'cached-b' and hit == 'true':
        selected = json.loads((EVIDENCE / 'selected-seed-cache.json').read_text())
        if os.environ.get('PILOT_RESTORED_KEY') != selected['key']:
            raise ValueError('restored cache is not the exact selected A key')
    comparison = cache_comparison(identity['mode'], hit, statistics.get('direct_cache_hit', 0) + statistics.get('preprocessed_cache_hit', 0))
    write('finish.json', {
        'finished_at': utc(), 'mode': identity['mode'], 'warm_cache_hit': hit,
        'comparison_status': comparison,
        'restore_step_wall_seconds': restore,
        'submission_to_phase_results_seconds': (datetime.now(timezone.utc) - datetime.fromisoformat(identity['submitted_at'])).total_seconds(),
        'provider_timing_limit': 'runner evidence start includes checkout; coordinator must retain final run/jobs API timestamps for exact queue/dispatch-to-completion', 'cache_bytes': cache_size, 'disk': output('df', '-h', '.'),
        'build_allocated': output('du', '-sk', str(BUILD)),
        'phase_rss_limit': 'GNU time max child RSS is not summed concurrent peak; no process-tree peak claim',
    })
    package()


def package():
    for p in EVIDENCE.rglob('*'):
        if p.is_file() and (p.is_symlink() or p.suffix not in ('.json', '.txt', '.tsv', '.xml', '.log')):
            raise ValueError(f'non-source evidence artifact rejected: {p.name}')
    files = {str(p.relative_to(EVIDENCE)): {'bytes': p.stat().st_size, 'sha256': sha(p)}
             for p in EVIDENCE.rglob('*') if p.is_file() and p.name != 'evidence-index.json'}
    if sum(v['bytes'] for v in files.values()) > 20 * 1024 * 1024:
        raise ValueError('source-only evidence exceeds 20 MiB artifact bound')
    write('evidence-index.json', files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('init', 'dependencies', 'configure', 'build', 'inventory', 'test', 'finish', 'package', 'empty-cache', 'cache-check'))
    args = parser.parse_args()
    if args.phase == 'init':
        init(os.environ['PILOT_EXPECTED_HEAD'], os.environ['PILOT_SUBMITTED_AT'], os.environ['PILOT_MODE'])
    elif args.phase == 'dependencies':
        measure('dependency-install', ['bash', '-euc',
            'sudo rm -f /etc/apt/sources.list.d/google-chrome.list /etc/apt/sources.list.d/google-chrome.sources; '
            'sudo apt-get update; sudo apt-get install -y cmake build-essential libssl-dev unixodbc-dev ccache time'], 600)
    elif args.phase == 'configure':
        configure()
    elif args.phase == 'empty-cache':
        empty_cache()
    elif args.phase == 'cache-check':
        check_seed_cache()
    elif args.phase == 'build':
        if os.environ.get('PILOT_MODE') in REAL_ROLES:
            os.environ['CCACHE_LOGFILE'] = str((EVIDENCE / 'compiler-cache.log').resolve())
        subprocess.run(['ccache', '--zero-stats'], check=True, timeout=30)
        measure('build', ['cmake', '--build', str(BUILD), '--parallel', '2', '--target',
                         *SUITES, 'odbcpp_sdk_header_isolation'], 1200)
    elif args.phase == 'inventory':
        inventory()
    elif args.phase == 'test':
        test()
    elif args.phase == 'finish':
        finish()
    else:
        package()


if __name__ == '__main__':
    main()
