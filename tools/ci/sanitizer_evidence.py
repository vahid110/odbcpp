#!/usr/bin/env python3
"""Cold sanitizer-only orchestration/evidence. Owning assertions remain GoogleTests."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET

FROZEN_PRODUCT = 'c82e4fe790c66cb9439d2f56c3b366163ffa3af7'
BUILD = Path('build-sanitize')
EVIDENCE = Path(os.environ.get('GITHUB_WORKSPACE', str(Path.cwd()))) / 'sanitizer-evidence'
PHASES = ('preflight', 'configure', 'build', 'inventory', 'test')
MAX_BYTES = 20 * 1024 * 1024
LOG_BYTES = 2 * 1024 * 1024
ASAN = 'detect_leaks=1:halt_on_error=1'
UBSAN = 'halt_on_error=1:print_stacktrace=1'
# The ordinary linux-sanitizers profile is unchanged. Export only adds metadata.
PROFILE = ('-DBUILD_EXAMPLES=OFF', '-DBUILD_TESTING=ON', '-DTARGET_DATABASE=POSTGRESQL',
           '-DODBCPP_CRYPTO_PROVIDER=OPENSSL', '-DODBCPP_CRYPTO_LINKAGE=SYSTEM_SHARED',
           '-DODBC_DRIVER_MANAGER_FLAVOR=UNIXODBC', '-DODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=2',
           '-DODBCPP_EXPECT_DM_SQLWCHAR_SIZE=2', '-DCMAKE_BUILD_TYPE=Debug',
           '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer',
           '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined',
           '-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined')


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def write(name, value):
    if EVIDENCE.is_symlink():
        raise ValueError('unsafe evidence root symlink')
    EVIDENCE.mkdir(exist_ok=True)
    (EVIDENCE / name).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def output(argv):
    return subprocess.check_output(argv, text=True, timeout=60).strip()


def immutable(value):
    if not re.fullmatch('[0-9a-f]{40}', value):
        raise ValueError('immutable lowercase commit required')
    return value


def request_guard(env):
    for key in ('SANITIZER_WORKFLOW_HEAD', 'SANITIZER_PRODUCT_HEAD', 'SANITIZER_PRODUCT_BASE'):
        immutable(env[key])
    if env['SANITIZER_WORKFLOW_HEAD'] == env['SANITIZER_PRODUCT_HEAD'] or env['SANITIZER_PRODUCT_HEAD'] != FROZEN_PRODUCT or env['GITHUB_SHA'] != env['SANITIZER_WORKFLOW_HEAD']:
        raise ValueError('separate exact W and frozen P, dispatched ref required')
    if env['SANITIZER_ONLY'] != 'true' or env['SANITIZER_CONFLICT'] != 'false' or json.loads(env['SANITIZER_CACHE']) != {}:
        raise ValueError('sanitizer-only is exclusive and cold')
    if (env['GITHUB_REPOSITORY'], env['GITHUB_REPOSITORY_VISIBILITY'], env['RUNNER_ENVIRONMENT'], env['RUNNER_OS'], env['RUNNER_ARCH']) != ('vahid110/odbcpp', 'public', 'github-hosted', 'Linux', 'X64'):
        raise ValueError('standard free public Linux x64 workflow required')
    workspace = Path(env['GITHUB_WORKSPACE']).resolve(strict=True)
    for key, child in [('SANITIZER_WORKFLOW_ROOT', 'sanitizer-workflow'), ('SANITIZER_PRODUCT_ROOT', 'sanitizer-product')]:
        if Path(env[key]).resolve() != workspace / child:
            raise ValueError('unsafe or substituted source/output path')
    if env['ASAN_OPTIONS'] != ASAN or env['UBSAN_OPTIONS'] != UBSAN:
        raise ValueError('sanitizer runtime flags changed')
    stamp = datetime.fromisoformat(env['SANITIZER_SUBMITTED_AT'].replace('Z', '+00:00'))
    if stamp.utcoffset() is None or stamp.utcoffset().total_seconds() != 0 or stamp > datetime.now(timezone.utc):
        raise ValueError('preceding UTC submission required')
    return stamp


def source(head, root):
    root = Path(root).resolve(strict=True)
    git = ['git', '-C', str(root)]
    if output([*git, 'rev-parse', 'HEAD']) != immutable(head) or output([*git, 'status', '--porcelain', '--untracked-files=no']):
        raise ValueError('immutable source changed')
    untracked = output([*git, 'ls-files', '--others']).splitlines()
    if any(not name.startswith((BUILD.as_posix() + '/', EVIDENCE.as_posix() + '/')) for name in untracked):
        raise ValueError('untracked source injection')
    files = output([*git, 'ls-files']).splitlines()
    return {'head': head, 'tree': output([*git, 'rev-parse', 'HEAD^{tree}']),
            'files': {name: sha(root / name) for name in files}}


def sources():
    return {'product': source(os.environ['SANITIZER_PRODUCT_HEAD'], os.environ['SANITIZER_PRODUCT_ROOT']),
            'workflow': source(os.environ['SANITIZER_WORKFLOW_HEAD'], os.environ['SANITIZER_WORKFLOW_ROOT'])}


def source_guard():
    before = json.loads((EVIDENCE / 'identity.json').read_text())['sources']
    after = sources()
    write('sources-after.json', after)
    if before != after:
        raise ValueError('W/P source before/after changed')


def group_active(pid):
    try:
        os.killpg(pid, 0)
        return True
    except ProcessLookupError:
        return False


def measure(name, argv, timeout, extra_env=None):
    EVIDENCE.mkdir(exist_ok=True)
    started = datetime.now(timezone.utc).isoformat()
    tick = time.monotonic()
    env = dict(os.environ, **(extra_env or {}))
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         start_new_session=True, env=env)
    head, tail, total = bytearray(), bytearray(), [0]
    def drain():
        for block in iter(lambda: p.stdout.read(8192), b''):
            total[0] += len(block)
            head.extend(block[:max(0, LOG_BYTES - len(head))])
            tail.extend(block)
            if len(tail) > 65536:
                del tail[:-65536]
    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    timed_out = False
    try:
        code = p.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        code = p.wait(timeout=5)
    unexpected = group_active(p.pid)
    cleanup_started = time.monotonic()
    cleanup_errors = []
    if unexpected:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError as exc:
            cleanup_errors.append(str(exc))
    while group_active(p.pid) and time.monotonic() - cleanup_started < 5:
        time.sleep(0.01)
    absent = not group_active(p.pid)
    reader.join(timeout=max(0, 5 - (time.monotonic() - cleanup_started)))
    if not reader.is_alive():
        p.stdout.close()
    raw = bytes(head)
    if total[0] > LOG_BYTES:
        raw += b'\n[bounded middle omitted; original final 64KiB follows]\n' + tail
    (EVIDENCE / (name + '.log')).write_bytes(raw)
    rec = {'argv': argv, 'exit': code, 'timed_out': timed_out, 'direct_pid': p.pid,
           'direct_terminal': p.poll() is not None, 'started_at': started,
           'finished_at': datetime.now(timezone.utc).isoformat(), 'elapsed_seconds': time.monotonic() - tick,
           'log_emitted_bytes': total[0], 'log_truncated': total[0] > LOG_BYTES,
           'pipe_drained': not reader.is_alive(), 'unexpected_group_children': unexpected,
           'cleanup': {'owned_process_group': p.pid, 'group_absent': absent, 'errors': cleanup_errors,
                       'elapsed_seconds': time.monotonic() - cleanup_started, 'deadline_seconds': 5},
           'maximum_child_RSS_KiB': resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
           'memory_scope': 'maximum child process RSS, not aggregate process-tree peak'}
    write(name + '-command.json', rec)
    if code or timed_out or unexpected or not absent or reader.is_alive() or cleanup_errors:
        raise RuntimeError(name + ' failed; original command/log/cleanup retained')
    return rec


def init():
    submitted = request_guard(os.environ)
    if BUILD.exists() or EVIDENCE.exists():
        raise ValueError('cold graph and evidence must be absent')
    write('identity.json', {'sources': sources(), 'base': os.environ['SANITIZER_PRODUCT_BASE'],
          'runner': {key: os.environ.get(key) for key in ('GITHUB_SHA', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'RUNNER_OS', 'RUNNER_ARCH', 'RUNNER_ENVIRONMENT')},
          'submitted_at': submitted.isoformat(), 'profile': list(PROFILE), 'ASAN_OPTIONS': ASAN, 'UBSAN_OPTIONS': UBSAN,
          'cache': {'restored': False, 'saved': False}, 'disk_free_bytes': shutil.disk_usage('.').free})


def preflight():
    base = immutable(os.environ['SANITIZER_PRODUCT_BASE'])
    output(['git', 'merge-base', '--is-ancestor', base, 'HEAD'])
    files = output(['git', 'diff', '--name-only', base, 'HEAD', '--', '*.cpp', '*.cc', '*.cxx', '*.h', '*.hpp']).splitlines()
    if files:
        measure('changed-gtest', [sys.executable, '-B', 'tools/development/check_changed_gtest_control_flow.py',
                                 '--base', base, '--head', os.environ['SANITIZER_PRODUCT_HEAD'], *files], 120)
    write('changed-gtest.json', {'base': base, 'head': os.environ['SANITIZER_PRODUCT_HEAD'], 'paths': files})


def configure():
    if BUILD.exists():
        raise ValueError('fresh sanitizer graph required')
    measure('configure', ['cmake', '-S', '.', '-B', str(BUILD), *PROFILE, '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'], 300)
    cache = (BUILD / 'CMakeCache.txt').read_text()
    for flag in PROFILE:
        key, value = flag[2:].split('=', 1)
        if not re.search(r'^' + re.escape(key) + r':[^=]+=' + re.escape(value) + r'$', cache, re.M):
            raise ValueError('actual sanitizer cache/profile differs: ' + key)
    shutil.copyfile(BUILD / 'CMakeCache.txt', EVIDENCE / 'CMakeCache.txt')
    compiler = re.search(r'^CMAKE_CXX_COMPILER:FILEPATH=(.*)$', cache, re.M)
    if not compiler:
        raise ValueError('actual compiler missing')
    path = Path(compiler[1]).resolve(strict=True)
    write('compiler.json', {'path': str(path), 'sha256': sha(path), 'version': output([str(path), '--version']),
          'cmake': output(['cmake', '--version']), 'ctest': output(['ctest', '--version'])})


def build():
    measure('build', ['cmake', '--build', str(BUILD), '--parallel', '2', '--verbose'], 1200)


def gtest_names(text):
    result, suite = [], ''
    for line in text.splitlines():
        bare = line.split('#', 1)[0].rstrip()
        if bare.startswith('  ') and suite:
            result.append(suite + bare.strip())
        elif bare and not bare.startswith(' ') and bare.endswith('.'):
            suite = bare
    if not result or len(result) != len(set(result)) or any('DISABLED_' in name for name in result):
        raise ValueError('nonempty exact enabled GoogleTest inventory required')
    return sorted(result)


def gtest_xml_path(name):
    if not re.fullmatch(r'test_[a-z0-9_]+', name):
        raise ValueError('unsafe or non-owning binary name')
    return EVIDENCE / 'gtest' / (name + '.xml')


def xml_root(path):
    raw = Path(path).read_bytes()
    if len(raw) > MAX_BYTES or b'<!DOCTYPE' in raw.upper() or b'<!ENTITY' in raw.upper():
        raise ValueError('unsafe XML')
    return ET.fromstring(raw)


def xml_result(path, expected):
    root = xml_root(path)
    cases = list(root.iter('testcase'))
    names = sorted(case.attrib['classname'] + '.' + case.attrib['name'] for case in cases)
    if names != sorted(expected) or len(names) != len(set(names)) or any(list(root.iter(tag)) for tag in ('failure', 'error', 'skipped')) or any(case.get('status') == 'notrun' or case.get('result') in ('skipped', 'suppressed') for case in cases):
        raise ValueError('owning XML failure/skip/inventory mismatch')
    if int(root.get('tests', len(cases))) != len(cases) or any(int(root.get(key, 0)) for key in ('failures', 'errors', 'disabled', 'skipped')):
        raise ValueError('declared XML failure or count mismatch')
    return {'count': len(cases), 'names': names, 'xml_sha256': sha(path)}


def require_unit_sources(ctest_names, unit_sources):
    if not set(unit_sources) <= set(ctest_names):
        raise ValueError('new source unit producer missing from actual CTest inventory')


def validate_compile_commands(commands):
    if not commands:
        raise ValueError('actual compiler commands missing')
    for command in commands:
        if Path(command['file']).suffix.lower() not in ('.cc', '.cpp', '.cxx'):
            continue
        args = command.get('arguments') or shlex.split(command['command'])
        if not {'-fsanitize=address,undefined', '-fno-omit-frame-pointer'} <= set(args) or any(arg.startswith('-fno-sanitize') for arg in args):
            raise ValueError('actual C++ sanitizer instrumentation differs')


def validate_sdk(headers, staged, objects):
    if len(headers) != 23 or staged != headers or len(objects) != 23:
        raise ValueError('SDK23 isolated header/object proof missing or changed')
    return {'headers': staged, 'objects': objects}


def inventory():
    listing = json.loads(output(['ctest', '--test-dir', str(BUILD), '-L', 'unit', '--show-only=json-v1']))
    entries = listing['tests']
    names = [entry['name'] for entry in entries]
    if not names or len(names) != len(set(names)):
        raise ValueError('exact nonempty all-unit CTest inventory required')
    # Four explicitly optional SDK consumers are excluded by the frozen CMake unit loop.
    optional = {'test_named_profile_acquisition', 'test_provisioned_native_owner', 'test_provisioned_pg_connector', 'test_provisioned_sdk_runtime'}
    require_unit_sources(names, [p.stem for p in Path('tests/unit').glob('*.cpp') if p.stem not in optional])
    write('ctest-inventory.json', listing)
    objects = {p.relative_to(BUILD).as_posix(): {'bytes': p.stat().st_size, 'sha256': sha(p)} for p in sorted(BUILD.rglob('*.o'))}
    links = {p.relative_to(BUILD).as_posix(): {'bytes': p.stat().st_size, 'sha256': sha(p)} for p in sorted(BUILD.rglob('*')) if p.is_file() and (p.suffix in ('.a', '.so') or p.parent == BUILD / 'tests' and os.access(p, os.X_OK))}
    if not objects or 'libodbcpp.so' not in links or not any(name.endswith('libodbcpp_core.a') for name in links):
        raise ValueError('fresh core/driver/objects missing')
    write('objects.json', objects)
    write('links.json', links)
    commands = json.loads((BUILD / 'compile_commands.json').read_text())
    validate_compile_commands(commands)
    dependencies = {}
    for command in commands:
        path = Path(command['file']).resolve(strict=True)
        command['source_sha256'] = sha(path)
        args = command.get('arguments') or shlex.split(command['command'])
        object_path = Path(command.get('output') or args[args.index('-o') + 1])
        if not object_path.is_absolute():
            object_path = Path(command['directory']) / object_path
        key = object_path.resolve(strict=True).relative_to(BUILD.resolve()).as_posix()
        if key not in objects:
            raise ValueError('compile command has no fresh bound object')
        command['object'] = {'path': key, **objects[key]}
    for path in BUILD.rglob('*.o.d'):
        raw = path.read_text().replace('\\\n', '')
        if ': ' not in raw:
            raise ValueError('dependency file target missing')
        for item in shlex.split(raw.split(': ', 1)[1]):
            dep = Path(item)
            if dep.is_file():
                dependencies[str(dep.resolve())] = sha(dep)
    if not commands or not dependencies:
        raise ValueError('actual full compiler commands/dependencies missing')
    write('compile-commands.json', commands)
    write('dependencies.json', dependencies)
    flags = {p.relative_to(BUILD).as_posix(): {'sha256': sha(p), 'text': p.read_text()} for pattern in ('flags.make', 'link.txt') for p in BUILD.rglob(pattern)}
    write('compiler-flags-links.json', flags)
    sdk = {name: entry for name, entry in objects.items() if 'odbcpp_sdk_header_isolation.dir' in name}
    headers = Path('sdk/contract_headers.txt').read_text().splitlines()
    original = {name: sha(Path('sdk/include') / name) for name in headers}
    staged = {name: sha(BUILD / 'sdk/contract-include' / name) for name in headers}
    write('sdk-header-isolation.json', validate_sdk(original, staged, sdk))
    binaries = {}
    for entry in entries:
        path = Path(entry['command'][0]).resolve(strict=True)
        candidates = list(BUILD.rglob(entry['name'] + '.dir/link.txt'))
        if not candidates or not any('gtest_main' in p.read_text() for p in candidates):
            continue  # Non-Google unit CTests are still required in the complete JUnit result.
        name = entry['name']
        gtest_xml_path(name)
        measure('list-' + name, [str(path), '--gtest_list_tests'], 60)
        listed = gtest_names((EVIDENCE / ('list-' + name + '.log')).read_text())
        binaries[name] = {'path': str(path), 'sha256': sha(path), 'names': listed}
    if not binaries or len({Path(v['path']).name for v in binaries.values()}) != len(binaries):
        raise ValueError('missing or colliding per-binary GoogleTest producers')
    require_unit_sources(binaries, [p.stem for p in Path('tests/unit').glob('*.cpp') if p.stem not in optional])
    write('executables.json', binaries)


def test():
    binaries = json.loads((EVIDENCE / 'executables.json').read_text())
    directory = (EVIDENCE / 'gtest').resolve()
    directory.mkdir()
    # A directory gives GoogleTest its executable's unique filename, never one shared XML file.
    measure('test', ['ctest', '--test-dir', str(BUILD), '-L', 'unit', '--timeout', '60', '--output-on-failure',
                     '--output-junit', str((EVIDENCE / 'ctest.xml').resolve())], 900,
            {'GTEST_OUTPUT': 'xml:' + directory.as_posix() + '/'})
    expected = [entry['name'] for entry in json.loads((EVIDENCE / 'ctest-inventory.json').read_text())['tests']]
    root = xml_root(EVIDENCE / 'ctest.xml')
    if sorted(case.attrib['name'] for case in root.iter('testcase')) != sorted(expected) or any(list(root.iter(tag)) for tag in ('failure', 'error', 'skipped')):
        raise ValueError('all-unit CTest result failure/missing/skip')
    if {p.name for p in directory.iterdir()} != {name + '.xml' for name in binaries}:
        raise ValueError('per-binary XML missing/extra/overwritten')
    results = {}
    for name, record in binaries.items():
        if sha(record['path']) != record['sha256']:
            raise ValueError('owning executable changed during test')
        results[name] = xml_result(gtest_xml_path(name), record['names'])
    write('results.json', results)


def completion():
    phases = {name: json.loads((EVIDENCE / (name + '.json')).read_text()) if (EVIDENCE / (name + '.json')).exists() else {'status': 'missing'} for name in PHASES}
    return all(v.get('status') == 'passed' for v in phases.values()) and (EVIDENCE / 'results.json').exists(), phases


def finish():
    source_guard()
    complete, phases = completion()
    write('finish.json', {'complete': complete, 'phases': phases, 'cache': {'restored': False, 'saved': False}, 'disk_free_bytes': shutil.disk_usage('.').free})
    if not complete:
        raise RuntimeError('incomplete sanitizer qualification; original failures preserved')


def package():
    if EVIDENCE.is_symlink():
        raise ValueError('unsafe evidence root symlink')
    EVIDENCE.mkdir(exist_ok=True)
    files = {}
    for p in sorted(EVIDENCE.rglob('*')):
        if p.is_symlink():
            raise ValueError('evidence symlink rejected')
        if p.is_file() and p.name != 'evidence-index.json':
            if p.suffix not in ('.json', '.txt', '.xml', '.log'):
                raise ValueError('only text evidence allowed')
            files[p.relative_to(EVIDENCE).as_posix()] = {'bytes': p.stat().st_size, 'sha256': sha(p)}
    encoded = json.dumps(files, indent=2, sort_keys=True) + '\n'
    if sum(v['bytes'] for v in files.values()) + len(encoded.encode()) > MAX_BYTES:
        raise ValueError('20MiB text bound exceeded including index')
    (EVIDENCE / 'evidence-index.json').write_text(encoded, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('request', 'init', *PHASES, 'finish', 'package'))
    phase = parser.parse_args().phase
    try:
        if phase == 'request':
            request_guard(os.environ)
            return
        if phase in PHASES:
            source_guard()
        globals()[phase]()
        if phase in PHASES:
            source_guard()
            write(phase + '.json', {'status': 'passed', 'finished_at': datetime.now(timezone.utc).isoformat()})
    except Exception as exc:
        path = EVIDENCE / (phase + '.json')
        record = json.loads(path.read_text()) if path.exists() else {}
        record.update(status='failed', error=str(exc), type=type(exc).__name__)
        write(phase + '.json', record)
        raise


if __name__ == '__main__':
    main()
