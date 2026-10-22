"""Offline orchestration safety checks; these are not Windows product proof."""
from copy import deepcopy
import ast
import importlib.util
import itertools
import json
import hashlib
import os
from pathlib import Path, PureWindowsPath
import subprocess
import tempfile
import ctypes
import queue
import threading
import time
from types import SimpleNamespace
from io import BytesIO
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('windows_product', Path(__file__).with_name('windows_product_qualification.py'))
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)


class RequestSafety(unittest.TestCase):
    def setUp(self):
        self.env = {'WINDOWS_WORKFLOW_HEAD': 'a' * 40, 'WINDOWS_PRODUCT_HEAD': w.FIRST_PRODUCT,
                    'WINDOWS_PRODUCT_BASE': 'b' * 40, 'WINDOWS_PRODUCT_TARGETS': 'test_backend_contract',
                    'WINDOWS_CONFLICTING_MODE': 'false', 'WINDOWS_CACHE_REQUEST': '{}',
                    'WINDOWS_SUBMITTED_AT': '2026-01-01T00:00:00+00:00',
                    'GITHUB_REPOSITORY': 'vahid110/odbcpp', 'GITHUB_REPOSITORY_VISIBILITY': 'public',
                    'RUNNER_ENVIRONMENT': 'github-hosted', 'RUNNER_OS': 'Windows', 'RUNNER_ARCH': 'X64'}

    def test_exact_cold_request_and_owning_selection(self):
        self.assertIsNotNone(w.request_guard(self.env))
        self.assertEqual(w.selection('test_backend_contract,test_diagnostics_apis'),
                         ['test_backend_contract', 'test_diagnostics_apis'])

    def test_parser_literal_selection_keeps_existing_targets_and_cold_guards(self):
        self.assertEqual(w.TARGETS, (
            'test_backend_contract', 'test_diagnostics_apis',
            'test_numeric_scale_conversion', 'test_redshift_data_conversion',
            'test_redshift_catalog_execution', 'test_sql_escape',
            'test_pg_protocol_parser'))
        self.assertEqual(w.selection('test_pg_protocol_parser'), ['test_pg_protocol_parser'])
        self.assertIsNotNone(w.request_guard(dict(
            self.env, WINDOWS_PRODUCT_HEAD='c' * 40,
            WINDOWS_PRODUCT_TARGETS='test_pg_protocol_parser')))

    def test_parser_selection_rejects_expansion_and_preserves_first_product_sentinel(self):
        for bad in ('test_pg_protocol_parser_unknown', 'test_pg_protocol_parser.*',
                    'test_pg_protocol_parser,test_pg_protocol_parser',
                    'test_pg_protocol_parser;whoami',
                    'test_pg_protocol_parser,test_backend_contract,test_diagnostics_apis,'
                    'test_numeric_scale_conversion,test_redshift_data_conversion'):
            with self.subTest(targets=bad), self.assertRaises(ValueError):
                w.selection(bad)
        with self.assertRaisesRegex(ValueError, 'first numeric product'):
            w.request_guard(dict(self.env, WINDOWS_PRODUCT_TARGETS='test_pg_protocol_parser'))
        self.assertIsNotNone(w.request_guard(dict(
            self.env, WINDOWS_PRODUCT_TARGETS='test_pg_protocol_parser,test_backend_contract')))

    def test_reject_mutable_refs_and_injected_or_duplicate_targets(self):
        for key in ('WINDOWS_WORKFLOW_HEAD', 'WINDOWS_PRODUCT_HEAD', 'WINDOWS_PRODUCT_BASE'):
            for bad in ('main', 'a' * 39, 'A' * 40, 'a' * 40 + '; echo bad'):
                env = dict(self.env, **{key: bad})
                with self.subTest(key=key, value=bad), self.assertRaises(ValueError):
                    w.request_guard(env)
        for bad in ('', 'test_backend_contract;whoami', '.*', 'all',
                    'test_backend_contract,test_backend_contract', ' test_backend_contract', ','.join(w.TARGETS)):
            with self.subTest(targets=bad), self.assertRaises(ValueError):
                w.selection(bad)

    def test_reject_conflicts_cache_private_paid_or_wrong_platform(self):
        for key, bad in (('WINDOWS_CONFLICTING_MODE', 'true'), ('WINDOWS_CACHE_REQUEST', '{"key":"x"}'),
                         ('GITHUB_REPOSITORY', 'some/private'), ('GITHUB_REPOSITORY_VISIBILITY', 'private'),
                         ('RUNNER_ENVIRONMENT', 'self-hosted'), ('RUNNER_ARCH', 'ARM64'),
                         ('RUNNER_OS', 'Linux'), ('WINDOWS_PRODUCT_TARGETS', 'test_diagnostics_apis'),
                         ('WINDOWS_SUBMITTED_AT', '2999-01-01T00:00:00Z'),
                         ('WINDOWS_SUBMITTED_AT', '2026-01-01T00:00:00')):
            with self.subTest(key=key, value=bad), self.assertRaises(ValueError):
                w.request_guard(dict(self.env, **{key: bad}))

    def test_source_identity_rejects_tracked_and_untracked_injection(self):
        responses = ['a' * 40, ' M core/x.cpp']
        with patch.object(w, 'output', side_effect=responses), self.assertRaises(ValueError):
            w.source('a' * 40)
        responses = ['a' * 40, '', 'core/injected.cpp']
        with patch.object(w, 'output', side_effect=responses), self.assertRaises(ValueError):
            w.source('a' * 40)

    def test_source_guard_checks_both_immutable_sources(self):
        before = {'product': {'head': 'a'}, 'workflow': {'head': 'b'}}
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            w.write('identity.json', {'sources': before})
            for role in ('product', 'workflow'):
                after = deepcopy(before)
                after[role]['head'] = 'changed'
                with patch.object(w, 'sources', return_value=after), self.assertRaises(ValueError):
                    w.source_guard()


class ResultAndFailureSafety(unittest.TestCase):
    def test_real_inventory_requires_cases_and_rejects_disabled(self):
        text = 'Running main() from gtest_main.cc\nBackendContractTest.\n  One\n  Two # TypeParam = x\n'
        self.assertEqual(w.gtest_names(text), ['BackendContractTest.One', 'BackendContractTest.Two'])
        for bad in ('', 'Empty.\n', 'Suite.\n  DISABLED_case\n', 'Suite.\n  One\n  One\n'):
            with self.subTest(value=bad), self.assertRaises(ValueError):
                w.gtest_names(bad)

    def test_xml_exact_inventory_failure_skip_and_missing_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'owning.xml'
            good = '<testsuites tests="1"><testsuite><testcase classname="Suite" name="One"/></testsuite></testsuites>'
            path.write_text(good)
            self.assertEqual(w.xml_result(path, ['Suite.One'])['count'], 1)
            for bad in (good.replace('/>', '><failure/></testcase>'),
                        good.replace('/>', '><skipped/></testcase>'),
                        good.replace('name="One"', 'name="Two"'),
                        good.replace('tests="1"', 'tests="2"'),
                        good.replace('name="One"', 'name="One" status="notrun"'),
                        good.replace('tests="1"', 'tests="1" failures="1"')):
                path.write_text(bad)
                with self.subTest(xml=bad), self.assertRaises(ValueError):
                    w.xml_result(path, ['Suite.One'])

    def test_failed_and_missing_phases_never_become_completion(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            complete, phases = w.completion()
            self.assertFalse(complete)
            self.assertTrue(all(v['status'] == 'missing' for v in phases.values()))
            for phase in w.PHASES:
                w.write(phase + '.json', {'status': 'passed'})
            self.assertFalse(w.completion()[0])  # Missing actual XML-derived results.
            w.write('results.json', {'owning': 'validated'})
            self.assertTrue(w.completion()[0])
            w.write('build.json', {'status': 'failed', 'error': 'original MSVC failure'})
            self.assertFalse(w.completion()[0])
            with patch.object(w, 'source_guard'), self.assertRaises(RuntimeError):
                w.finish()
            self.assertEqual(json.loads((Path(tmp) / 'build.json').read_text())['error'], 'original MSVC failure')

    def test_failed_command_preserves_exit_and_log(self):
        # Execute only a trivial local offline child, never a product/helper phase.
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            with self.assertRaises(RuntimeError):
                w.measure('failure', [os.sys.executable, '-c', 'print("original diagnostic"); raise SystemExit(7)'], 10)
            record = json.loads((Path(tmp) / 'failure-command.json').read_text())
            self.assertEqual(record['exit'], 7)
            self.assertTrue(record['direct_terminal'])
            self.assertIn('original diagnostic', (Path(tmp) / 'failure.log').read_text())

    def test_truncated_log_keeps_original_final_diagnostic(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)), patch.object(w, 'LOG_BYTES', 16):
            with self.assertRaises(RuntimeError):
                w.measure('large', [os.sys.executable, '-c', 'print("x" * 100); print("final original error"); raise SystemExit(9)'], 10)
            self.assertIn('final original error', (Path(tmp) / 'large.log').read_text())
            self.assertTrue(json.loads((Path(tmp) / 'large-command.json').read_text())['log_truncated'])

    def test_timeout_preserves_failure_and_terminal(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            with self.assertRaises(RuntimeError):
                w.measure('timeout', [os.sys.executable, '-c', 'import time; time.sleep(10)'], 0.05)
            record = json.loads((Path(tmp) / 'timeout-command.json').read_text())
            self.assertTrue(record['timed_out'])
            self.assertTrue(record['direct_terminal'])

    def test_artifact_includes_index_in_bound_and_rejects_binary_or_symlink(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            w.write('a.json', {'text': 'evidence'})
            w.package()
            self.assertIn('a.json', json.loads((Path(tmp) / 'evidence-index.json').read_text()))
            with patch.object(w, 'MAX_BYTES', 10), self.assertRaises(ValueError):
                w.package()
            binary = Path(tmp) / 'restored.exe'
            binary.write_bytes(b'MZ')
            with self.assertRaises(ValueError):
                w.package()
            binary.unlink()
            link = Path(tmp) / 'link.txt'
            try:
                link.symlink_to(Path(tmp) / 'a.json')
            except OSError:
                return  # Windows non-admin symlink permission is not a product result.
            with self.assertRaises(ValueError):
                w.package()


class ReviewRepairSafety(unittest.TestCase):
    def test_windows_inventory_keys_match_literal_driver_and_sdk_paths(self):
        root = PureWindowsPath(r'C:\work\build-windows-product')
        driver = PureWindowsPath(r'C:\work\build-windows-product\Release\odbcpp.dll')
        sdk = PureWindowsPath(r'C:\work\build-windows-product\sdk\odbcpp_sdk_header_isolation.dir\Release\one.obj')
        self.assertEqual(w.relative_key(driver, root), 'Release/odbcpp.dll')
        self.assertIn('Release/odbcpp.dll', {w.relative_key(driver, root): 'fresh DLL'})
        self.assertEqual(w.relative_key(sdk, root), 'sdk/odbcpp_sdk_header_isolation.dir/Release/one.obj')

    def test_assignment_precedes_resume_and_immediate_child_execution(self):
        events = ['created_suspended']
        process = SimpleNamespace(launch_order=events)
        def attach(p):
            self.assertNotIn('resumed', events)
            p.assigned = True
        def resume():
            self.assertTrue(process.assigned)
            self.assertEqual(events, ['created_suspended', 'assigned_to_job'])
            events.append('resumed')
        process.resume = resume
        w.assign_and_resume(SimpleNamespace(attach=attach), process)
        self.assertEqual(events, ['created_suspended', 'assigned_to_job', 'resumed'])

    def test_assignment_and_resume_failures_reap_without_success(self):
        for failing_method in ('attach', 'resume'):
            with self.subTest(method=failing_method), tempfile.TemporaryDirectory() as tmp:
                events = []
                class Process:
                    pid = 123
                    def __init__(self, command):
                        self.launch_order = ['created_suspended']
                        self.stdout = BytesIO()
                        self.terminal = False
                    def resume(self):
                        if failing_method == 'resume':
                            raise RuntimeError('original resume failure')
                        events.append('resumed')
                    def kill(self):
                        events.append('kill')
                        self.terminal = True
                    def wait(self, timeout):
                        self.assert_terminal = self.terminal
                        events.append('wait')
                        return 124
                    def poll(self):
                        return 124 if self.terminal else None
                    def close(self):
                        events.append('close_process')
                class Job:
                    def attach(self, process):
                        if failing_method == 'attach':
                            raise RuntimeError('original attach failure')
                    def active(self):
                        return 0
                    def close(self):
                        events.append('close_job')
                with patch.object(w, 'os', SimpleNamespace(name='nt')), patch.object(w, 'EVIDENCE', Path(tmp)), \
                     patch.object(w, 'WindowsSuspendedProcess', Process), patch.object(w, 'WindowsProcessJob', Job):
                    with self.assertRaisesRegex(RuntimeError, 'original ' + failing_method + ' failure'):
                        w.measure('launch', ['command'], 10)
                record = json.loads((Path(tmp) / 'launch-command.json').read_text())
                self.assertEqual(record['status'], 'failed')
                self.assertTrue(record['cleanup']['direct_terminal'])
                self.assertEqual(record['cleanup']['job_active_after_cleanup'], 0)
                self.assertNotIn('resumed', events)
                self.assertEqual(events, ['kill', 'wait', 'close_process', 'close_job'])


class ConfigureAuxiliarySafety(unittest.TestCase):
    def facts(self):
        compiler = 'C:/VS/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64/cl.exe'
        auxiliary = compiler.replace('/cl.exe', '/vctip.exe')
        context = {'installation_root': 'C:/VS', 'compiler_path': compiler,
                   'compiler_sha256': 'a' * 64, 'auxiliary_path': auxiliary,
                   'auxiliary_sha256': 'b' * 64, 'cache_sha256': 'c' * 64,
                   'compiler_configuration_sha256': 'd' * 64, 'profile': list(w.PROFILE)}
        child = {'pid': 6392, 'image_path': auxiliary, 'image_sha256': 'b' * 64,
                 'creation_filetime': 134364503295670202, 'creation_unit': '100ns since1601 UTC',
                 'job_member': True, 'held_handle': True, 'terminal_after_cleanup': True,
                 'held_handle_closed': True}
        record = {'phase': 'configure', 'argv': ['cmake', '-S', '.', '-B', str(w.BUILD), *w.PROFILE],
                  'exit': 0, 'timed_out': False, 'direct_terminal': True, 'direct_pid': 1212,
                  'started_at': '2026-10-14T11:18:42.669203+00:00',
                  'direct_exited_at': '2026-10-14T11:23:02.829463+00:00',
                  'launch_order': ['created_suspended', 'assigned_to_job', 'resumed'],
                  'fresh_graph_before_launch': True, 'pipe_drained': True,
                  'unexpected_children': True,
                  'cleanup': {'active_after_grace': 1, 'active_after_cleanup': 0,
                              'tree_owned_by_job': True, 'kill_on_job_close': True,
                              'excluded_direct_pid': 1212, 'owned_descendants': [child],
                              'identity_errors': [], 'termination_requested': True,
                              'termination_succeeded': True, 'cleanup_seconds': 5,
                              'cleanup_elapsed_seconds': 0.046,
                              'identity_stages': [{'stage': 'after_grace', 'member_pids': [6392]}]}}
        return record, context

    def test_exact_configure_auxiliary_cleanup_is_named_and_raw_failure_fact_survives(self):
        record, context = self.facts()
        result = w.configure_auxiliary_disposition(record, context)
        self.assertTrue(result['accepted'])
        self.assertEqual(result['disposition'], 'expected_owned_compiler_auxiliary_cleaned')
        self.assertTrue(record['unexpected_children'])
        self.assertEqual(record['cleanup']['active_after_grace'], 1)

    def test_default_other_phases_direct_failure_and_missing_proof_stay_refused(self):
        record, context = self.facts()
        for field, bad in [('phase', 'build'), ('phase', 'test'), ('phase', 'preflight'),
                           ('phase', 'owned-child'), ('exit', 1), ('exit', False), ('timed_out', True),
                           ('direct_terminal', False), ('pipe_drained', False),
                           ('fresh_graph_before_launch', False), ('argv', ['cmake', '--version']),
                           ('launch_order', ['popen']), ('direct_exited_at', None)]:
            with self.subTest(field=field, bad=bad):
                changed = deepcopy(record)
                changed[field] = bad
                self.assertFalse(w.configure_auxiliary_disposition(changed, context)['accepted'])
        self.assertFalse(w.configure_auxiliary_disposition(record, None)['accepted'])
        self.assertFalse(w.configure_auxiliary_disposition({}, context)['accepted'])

    def test_other_mixed_unknown_image_hash_creation_and_membership_stay_refused(self):
        record, context = self.facts()
        for field, bad in [('image_path', 'C:/other/vctip.exe'), ('image_path', 'C:/VS-evil/vctip.exe'),
                           ('image_path', context['auxiliary_path'].replace('Hostx64', 'Hostx86')),
                           ('image_sha256', 'e' * 64), ('image_sha256', None),
                           ('creation_filetime', 0), ('creation_filetime', 1),
                           ('creation_filetime', 999999999999999999), ('creation_unit', None),
                           ('job_member', False), ('held_handle', False), ('pid', 1212)]:
            with self.subTest(field=field, bad=bad):
                changed = deepcopy(record)
                changed['cleanup']['owned_descendants'][0][field] = bad
                self.assertFalse(w.configure_auxiliary_disposition(changed, context)['accepted'])
        changed = deepcopy(record)
        changed['cleanup']['owned_descendants'].append({'pid': 123, 'image_path': 'C:/python.exe'})
        self.assertFalse(w.configure_auxiliary_disposition(changed, context)['accepted'])

    def test_termination_shared_deadline_wait_close_query_and_nonzero_refuse(self):
        record, context = self.facts()
        for field, bad in [('active_after_cleanup', 1), ('identity_errors', [{'error': 'query failed'}]),
                           ('termination_requested', False), ('termination_succeeded', False),
                           ('cleanup_seconds', 6), ('cleanup_elapsed_seconds', 5.01),
                           ('identity_stages', []), ('active_after_grace', 2),
                           ('active_after_cleanup', False), ('cleanup_elapsed_seconds', float('nan'))]:
            with self.subTest(field=field):
                changed = deepcopy(record)
                changed['cleanup'][field] = bad
                self.assertFalse(w.configure_auxiliary_disposition(changed, context)['accepted'])
        for field in ['terminal_after_cleanup', 'held_handle_closed']:
            for bad in [False, None]:
                changed = deepcopy(record)
                changed['cleanup']['owned_descendants'][0][field] = bad
                self.assertFalse(w.configure_auxiliary_disposition(changed, context)['accepted'])
        for field in context:
            changed = dict(context)
            del changed[field]
            self.assertFalse(w.configure_auxiliary_disposition(record, changed)['accepted'])
        changed = deepcopy(record)
        changed['cleanup']['identity_stages'][0]['member_pids'] = [9999]
        self.assertFalse(w.configure_auxiliary_disposition(changed, context)['accepted'])

    def test_measure_permits_only_private_configure_context_and_preserves_raw_fact(self):
        facts, context = self.facts()
        for phase, code, supplied in [('configure', 0, True), ('configure', 0, False),
                                      ('configure', 1, True), ('build', 0, True),
                                      ('owned-child', 0, True)]:
            with self.subTest(phase=phase, code=code, supplied=supplied), tempfile.TemporaryDirectory() as tmp:
                class Process:
                    pid, _handle = 1212, 1
                    def __init__(self, argv):
                        self.stdout = BytesIO(b'original command output\n')
                        self.launch_order = ['created_suspended']
                    def resume(self): self.launch_order.append('resumed')
                    def wait(self, timeout): return code
                    def poll(self): return code
                    def close(self): pass
                class Job:
                    def attach(self, process): pass
                    def close(self): pass
                accepted = phase == 'configure' and code == 0 and supplied
                with patch.object(w, 'os', SimpleNamespace(name='nt')), \
                     patch.object(w, 'EVIDENCE', Path(tmp) / 'evidence'), \
                     patch.object(w, 'BUILD', Path(tmp) / 'cold-graph'), \
                     patch.object(w, 'WindowsSuspendedProcess', Process), \
                     patch.object(w, 'WindowsProcessJob', Job), patch.object(w, 'direct_peak', return_value=1), \
                     patch.object(w, 'utc', side_effect=[facts['started_at'], facts['direct_exited_at'], facts['direct_exited_at']]), \
                     patch.object(w, 'owned_exit_cleanup', return_value=(deepcopy(facts['cleanup']), True, False)), \
                     patch.object(w, 'configure_auxiliary_context', return_value=context) as validator:
                    kwargs = {'configure_installation': {'private': 'installation'}} if supplied else {}
                    if accepted:
                        w.measure(phase, w.configure_command(), 10, **kwargs)
                    else:
                        with self.assertRaisesRegex(RuntimeError, 'command failed'):
                            w.measure(phase, w.configure_command(), 10, **kwargs)
                    self.assertEqual(validator.call_count, int(accepted))
                record = json.loads((Path(tmp) / 'evidence' / (phase + '-command.json')).read_text())
                self.assertEqual(record['exit'], code)
                self.assertTrue(record['unexpected_children'])
                self.assertEqual(record['cleanup']['active_after_grace'], 1)
                self.assertEqual(record['auxiliary_disposition']['accepted'], accepted)

    def graph(self, root):
        install = root / 'VS'
        tool = install / 'VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64'
        tool.mkdir(parents=True)
        compiler, auxiliary = tool / 'cl.exe', tool / 'vctip.exe'
        compiler.write_bytes(b'offline compiler identity')
        auxiliary.write_bytes(b'offline auxiliary identity')
        build = root / 'build'
        config = build / 'CMakeFiles/4.2/CMakeCXXCompiler.cmake'
        config.parent.mkdir(parents=True)
        config.write_text('set(CMAKE_CXX_COMPILER_ID "MSVC")\nset(CMAKE_CXX_COMPILER "' + compiler.as_posix() + '")\n')
        cache = {'CMAKE_GENERATOR_PLATFORM': 'x64', 'CMAKE_CONFIGURATION_TYPES': 'Debug;Release',
                 'CMAKE_GENERATOR': 'Visual Studio 18 2026'}
        cache.update(dict(flag[2:].split('=', 1) for flag in w.PROFILE if flag.startswith('-D')))
        (build / 'CMakeCache.txt').write_text(''.join(k + ':STRING=' + v + '\n' for k, v in cache.items()))
        installation = {'installationPath': str(install), 'resolvedInstallationPath': str(install),
                        'isComplete': True, 'isLaunchable': True}
        return installation, build, compiler, auxiliary, config

    def test_context_binds_generated_msvc_cache_and_exact_sibling_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            installation, build, compiler, auxiliary, config = self.graph(Path(tmp))
            context = w.configure_auxiliary_context(installation, build)
            self.assertEqual(context['compiler_path'], compiler.resolve().as_posix())
            self.assertEqual(context['auxiliary_path'], auxiliary.resolve().as_posix())
            self.assertEqual(context['compiler_sha256'], w.sha(compiler))
            self.assertEqual(context['auxiliary_sha256'], w.sha(auxiliary))
            self.assertEqual(context['compiler_configuration_sha256'], w.sha(config))

    def test_context_refuses_false_root_missing_cache_wrong_profile_and_compiler(self):
        for defect in ['root', 'incomplete', 'cache', 'profile', 'compiler', 'auxiliary',
                       'other_tool_root', 'aux_redirect', 'duplicate_cache', 'duplicate_compiler']:
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as tmp:
                installation, build, compiler, auxiliary, config = self.graph(Path(tmp))
                if defect == 'root':
                    installation['installationPath'] += '-evil'
                elif defect == 'incomplete':
                    installation['isComplete'] = False
                elif defect == 'cache':
                    (build / 'CMakeCache.txt').unlink()
                elif defect == 'profile':
                    cache = build / 'CMakeCache.txt'
                    cache.write_text(cache.read_text().replace('BUNDLED_SHARED', 'SYSTEM_SHARED'))
                elif defect == 'compiler':
                    config.write_text(config.read_text().replace('"MSVC"', '"GNU"'))
                elif defect == 'auxiliary':
                    auxiliary.unlink()
                elif defect == 'other_tool_root':
                    foreign = Path(tmp) / 'VS-evil' / 'cl.exe'
                    foreign.parent.mkdir()
                    foreign.write_bytes(b'foreign')
                    config.write_text(config.read_text().replace(compiler.as_posix(), foreign.as_posix()))
                elif defect == 'aux_redirect':
                    foreign = Path(tmp) / 'other-vctip.exe'
                    foreign.write_bytes(b'foreign')
                elif defect == 'duplicate_cache':
                    cache = build / 'CMakeCache.txt'
                    cache.write_text(cache.read_text() + 'TARGET_DATABASE:STRING=POSTGRESQL\n')
                elif defect == 'duplicate_compiler':
                    duplicate = build / 'CMakeFiles/other/CMakeCXXCompiler.cmake'
                    duplicate.parent.mkdir()
                    duplicate.write_text(config.read_text())
                original_resolve = Path.resolve
                redirected_auxiliary = original_resolve(auxiliary) if defect == 'aux_redirect' else None
                def resolve(path, *args, **kwargs):
                    resolved = original_resolve(path, *args, **kwargs)
                    return original_resolve(foreign, *args, **kwargs) if resolved == redirected_auxiliary else resolved
                # Exercise redirected canonical identity without requiring Windows symlink privileges.
                with patch.object(w.Path, 'resolve', autospec=True, side_effect=resolve), \
                     self.assertRaises((ValueError, OSError)):
                    w.configure_auxiliary_context(installation, build)


class BuildAuxiliarySafety(unittest.TestCase):
    def facts(self):
        record, tool = ConfigureAuxiliarySafety().facts()
        env = RequestSafety(); env.setUp()
        self.env = dict(env.env, WINDOWS_WORKFLOW_ROOT=str(ROOT), GITHUB_RUN_ID='123', GITHUB_RUN_ATTEMPT='1',
                        GITHUB_SHA=env.env['WINDOWS_WORKFLOW_HEAD'])
        self.sources = {name: {'head': self.env[key], 'tree': 'f' * 40, 'tracked_sha256': 'e' * 64}
                        for name, key in [('workflow', 'WINDOWS_WORKFLOW_HEAD'), ('product', 'WINDOWS_PRODUCT_HEAD')]}
        command = ['cmake', '--build', str(w.BUILD), '--config', 'Release', '--parallel', '2', '--target',
                   'test_backend_contract', 'odbcpp_sdk_header_isolation', 'odbcpp_driver', '--',
                   '/verbosity:normal', '/nodeReuse:false', '/p:TrackFileAccess=true']
        context = {'tool': tool, 'sources': deepcopy(self.sources), 'base': self.env['WINDOWS_PRODUCT_BASE'],
                   'argv': command, 'build_path': str(w.BUILD.resolve()),
                   'runner': {'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '1', 'GITHUB_SHA': self.env['GITHUB_SHA']},
                   'configure_finished_at': '2026-10-14T11:18:00+00:00',
                   'identity_sha256': '1' * 64, 'configure_record_sha256': '2' * 64,
                   'configure_phase_sha256': '3' * 64, 'dependencies_sha256': '4' * 64}
        record.update(phase='build', argv=command, fresh_graph_before_launch=False,
                      build_auxiliary_context=context, build_auxiliary_context_after=deepcopy(context),
                      build_auxiliary_context_error=None)
        return record, context

    def test_exact_build_cleanup_requires_context_and_retains_raw_fact(self):
        record, context = self.facts()
        with patch.dict(os.environ, self.env):
            result = w.build_auxiliary_disposition(record, context)
        self.assertTrue(result['accepted'])
        self.assertEqual(result['disposition'], 'expected_owned_compiler_auxiliary_cleaned')
        self.assertIn('build-only', result['policy'])
        self.assertTrue(record['unexpected_children'])
        self.assertFalse(record['fresh_graph_before_launch'])
        self.assertFalse(w.configure_auxiliary_disposition(record, context['tool'])['accepted'])

    def test_build_phase_argv_source_identity_and_context_mutations_refuse(self):
        record, context = self.facts()
        mutations = [('phase', 'configure'), ('phase', 'test'), ('phase', 'list'), ('phase', 'safety'),
                     ('argv', ['cmake', '--build', 'other']), ('exit', 1), ('exit', False),
                     ('timed_out', True), ('direct_terminal', False), ('pipe_drained', False),
                     ('fresh_graph_before_launch', True), ('build_auxiliary_context', None),
                     ('build_auxiliary_context_after', None), ('build_auxiliary_context_error', 'changed')]
        with patch.dict(os.environ, self.env):
            for key, value in mutations:
                with self.subTest(key=key, value=value):
                    changed = deepcopy(record); changed[key] = value
                    self.assertFalse(w.build_auxiliary_disposition(changed, context)['accepted'])
            for key, value in [('base', '0' * 40), ('argv', record['argv'][:-1]), ('build_path', 'other'),
                               ('configure_finished_at', record['direct_exited_at']),
                               ('sources', {}), ('runner', {}), ('tool', {})]:
                with self.subTest(context=key):
                    changed = deepcopy(context)
                    changed[key] = value
                    bound = deepcopy(record)
                    bound['build_auxiliary_context'] = deepcopy(changed)
                    bound['build_auxiliary_context_after'] = deepcopy(changed)
                    self.assertFalse(w.build_auxiliary_disposition(bound, changed)['accepted'])
            for key in context:
                changed = deepcopy(context)
                del changed[key]
                bound = deepcopy(record)
                bound['build_auxiliary_context'] = deepcopy(changed)
                bound['build_auxiliary_context_after'] = deepcopy(changed)
                self.assertFalse(w.build_auxiliary_disposition(bound, changed)['accepted'])
            self.assertFalse(w.build_auxiliary_disposition(record, None)['accepted'])

    def test_build_held_identity_mixed_unknown_and_cleanup_refusals(self):
        record, context = self.facts()
        with patch.dict(os.environ, self.env):
            for key, value in [('image_path', 'C:/unknown/vctip.exe'), ('image_sha256', '0' * 64),
                               ('creation_filetime', 0), ('job_member', False), ('held_handle', False),
                               ('terminal_after_cleanup', False), ('held_handle_closed', False)]:
                changed = deepcopy(record); changed['cleanup']['owned_descendants'][0][key] = value
                self.assertFalse(w.build_auxiliary_disposition(changed, context)['accepted'])
            for key, value in [('active_after_cleanup', 1), ('active_after_grace', None),
                               ('identity_errors', [{'error': 'wait/close/query failed'}]),
                               ('termination_requested', False), ('termination_succeeded', False),
                               ('cleanup_seconds', 6), ('cleanup_elapsed_seconds', 5.01),
                               ('identity_stages', [])]:
                changed = deepcopy(record); changed['cleanup'][key] = value
                self.assertFalse(w.build_auxiliary_disposition(changed, context)['accepted'])
            changed = deepcopy(record)
            changed['cleanup']['owned_descendants'].append({'pid': 55, 'image_path': 'C:/python.exe'})
            self.assertFalse(w.build_auxiliary_disposition(changed, context)['accepted'])

    def fixture(self, root):
        record, context = self.facts()
        evidence = root / 'evidence'; evidence.mkdir()
        build = root / 'graph'; build.mkdir()
        configured = deepcopy(record)
        configured.update(phase='configure', argv=w.configure_command(), fresh_graph_before_launch=True,
                          direct_exited_at='2026-10-14T11:17:59+00:00',
                          started_at='2026-10-14T11:16:00+00:00', unexpected_children=False,
                          configure_auxiliary_context=context['tool'], configure_auxiliary_context_error=None)
        configured['cleanup'].update(active_after_grace=0, active_after_cleanup=0,
                                     owned_descendants=[], identity_errors=[])
        identity = {'sources': self.sources, 'base': self.env['WINDOWS_PRODUCT_BASE'],
                    'targets': ['test_backend_contract'], 'profile': list(w.PROFILE),
                    'cache': {'mode': 'cold', 'restored': False, 'saved': False}, 'runner': context['runner'],
                    'product_path': str(Path.cwd()), 'workflow_path': str(ROOT)}
        values = {'identity.json': identity, 'configure-command.json': configured,
                  'configure.json': {'status': 'passed', 'finished_at': context['configure_finished_at']},
                  'dependencies.json': {'visual_studio': [{'selected': 'installation'}]}}
        for name, value in values.items():
            (evidence / name).write_text(json.dumps(value))
        return evidence, build, context, values

    def test_context_requires_same_fresh_successful_configure_and_revalidates(self):
        for defect in [None, 'failed', 'missing', 'nonzero', 'not_fresh', 'source', 'base', 'profile',
                       'targets', 'cache', 'runner', 'tool', 'cleanup', 'context_error']:
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as tmp:
                evidence, build, context, values = self.fixture(Path(tmp))
                if defect == 'failed': values['configure.json']['status'] = 'failed'
                if defect == 'nonzero': values['configure-command.json']['exit'] = 1
                if defect == 'not_fresh': values['configure-command.json']['fresh_graph_before_launch'] = False
                if defect == 'source': values['identity.json']['sources']['product']['head'] = '0' * 40
                if defect == 'base': values['identity.json']['base'] = '0' * 40
                if defect == 'profile': values['identity.json']['profile'] = []
                if defect == 'targets': values['identity.json']['targets'] = ['test_sql_escape']
                if defect == 'cache': values['identity.json']['cache']['restored'] = True
                if defect == 'runner': values['identity.json']['runner']['GITHUB_RUN_ID'] = 'other'
                if defect == 'cleanup': values['configure-command.json']['cleanup']['active_after_cleanup'] = 1
                if defect == 'context_error': values['configure-command.json']['configure_auxiliary_context_error'] = 'query failed'
                for name, value in values.items(): (evidence / name).write_text(json.dumps(value))
                if defect == 'missing': (evidence / 'configure.json').unlink()
                tool = deepcopy(context['tool'])
                if defect == 'tool': tool['compiler_sha256'] = '9' * 64
                actual_sources = deepcopy(context['sources'])
                with patch.dict(os.environ, self.env), patch.object(w, 'EVIDENCE', evidence), \
                     patch.object(w, 'BUILD', build), patch.object(w, 'sources', return_value=actual_sources), \
                     patch.object(w, 'configure_auxiliary_context', return_value=tool):
                    # The configure argv is fixed to the same physical graph for this fixture.
                    configured = values['configure-command.json']; configured['argv'][4] = str(build)
                    (evidence / 'configure-command.json').write_text(json.dumps(configured))
                    if defect is None:
                        result = w.build_auxiliary_context()
                        self.assertEqual(result['tool'], tool)
                        self.assertEqual(result['sources'], actual_sources)
                        self.assertEqual(result['argv'], w.build_command())
                        self.assertEqual(result['build_path'], str(build.resolve()))
                    else:
                        with self.assertRaises((ValueError, OSError)):
                            w.build_auxiliary_context()

    def test_live_tool_cache_config_and_checkout_paths_cannot_be_substituted(self):
        for defect in [None, 'compiler', 'auxiliary', 'cache', 'configuration', 'product_path', 'workflow_path']:
            with self.subTest(defect=defect), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                evidence, unused_build, context, values = self.fixture(root)
                installation, build, compiler, auxiliary, configuration = ConfigureAuxiliarySafety().graph(root)
                self.env['WINDOWS_WORKFLOW_ROOT'] = str(ROOT)
                values['identity.json'].update(product_path=str(Path.cwd()), workflow_path=str(ROOT))
                values['dependencies.json']['visual_studio'] = [installation]
                values['configure-command.json']['argv'][4] = str(build)
                values['configure-command.json']['configure_auxiliary_context'] = w.configure_auxiliary_context(installation, build)
                if defect in ['compiler', 'auxiliary', 'cache', 'configuration']:
                    target = {'compiler': compiler, 'auxiliary': auxiliary, 'cache': build / 'CMakeCache.txt',
                              'configuration': configuration}[defect]
                    target.write_bytes(target.read_bytes() + b'\nmodified bytes\n')
                if defect in ['product_path', 'workflow_path']:
                    values['identity.json'][defect] = str(root / 'other-checkout')
                for name, value in values.items(): (evidence / name).write_text(json.dumps(value))
                with patch.dict(os.environ, self.env), patch.object(w, 'EVIDENCE', evidence), \
                     patch.object(w, 'BUILD', build), patch.object(w, 'sources', return_value=context['sources']):
                    if defect is None:
                        self.assertEqual(w.build_auxiliary_context()['build_path'], str(build.resolve()))
                    else:
                        with self.assertRaises((ValueError, OSError)):
                            w.build_auxiliary_context()

    def test_missing_context_refuses_before_command_creation(self):
        record, context = self.facts()
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, self.env), \
             patch.object(w, 'EVIDENCE', Path(tmp)), \
             patch.object(w, 'build_auxiliary_context', side_effect=ValueError('configure missing')), \
             patch.object(w, 'WindowsSuspendedProcess') as process, patch.object(w.subprocess, 'Popen') as popen:
            with self.assertRaisesRegex(ValueError, 'configure missing'):
                w.measure('build', record['argv'], 10, owning_build=True)
            process.assert_not_called()
            popen.assert_not_called()
            failed = json.loads((Path(tmp) / 'build-command.json').read_text())
            self.assertEqual(failed['status'], 'failed')
            self.assertEqual(failed['build_auxiliary_context_error'], 'configure missing')

    def test_measure_revalidates_private_build_context_and_refuses_changes(self):
        record, context = self.facts()
        for private, changed in [(True, False), (False, False), (True, True), (True, 'query-failure')]:
            with self.subTest(private=private, changed=changed), tempfile.TemporaryDirectory() as tmp:
                class Process:
                    pid, _handle = 1212, 1
                    def __init__(self, argv):
                        self.stdout = BytesIO(b'build completed\n'); self.launch_order = ['created_suspended']
                    def resume(self): self.launch_order.append('resumed')
                    def wait(self, timeout): return 0
                    def poll(self): return 0
                    def close(self): pass
                class Job:
                    def attach(self, process): pass
                    def close(self): pass
                after = deepcopy(context)
                if changed == 'query-failure':
                    after = ValueError('tool query failed after direct exit')
                elif changed:
                    after['tool']['cache_sha256'] = '0' * 64
                with patch.dict(os.environ, self.env), patch.object(w, 'os', SimpleNamespace(name='nt', environ=os.environ)), \
                     patch.object(w, 'EVIDENCE', Path(tmp)), \
                     patch.object(w, 'WindowsSuspendedProcess', Process), patch.object(w, 'WindowsProcessJob', Job), \
                     patch.object(w, 'direct_peak', return_value=1), \
                     patch.object(w, 'utc', side_effect=[record['started_at'], record['direct_exited_at'], record['direct_exited_at']]), \
                     patch.object(w, 'owned_exit_cleanup', return_value=(deepcopy(record['cleanup']), True, False)), \
                     patch.object(w, 'build_auxiliary_context', side_effect=[context, after]) as validator:
                    if private and not changed:
                        w.measure('build', record['argv'], 10, owning_build=True)
                    else:
                        with self.assertRaisesRegex(RuntimeError, 'command failed'):
                            w.measure('build', record['argv'], 10, owning_build=private)
                    self.assertEqual(validator.call_count, 2 if private else 0)
                measured = json.loads((Path(tmp) / 'build-command.json').read_text())
                self.assertEqual(measured['exit'], 0)
                self.assertEqual(measured['cleanup']['active_after_cleanup'], 0)
                self.assertTrue(measured['cleanup']['owned_descendants'][0]['held_handle_closed'])
                self.assertTrue(measured['unexpected_children'])
                self.assertEqual(measured['auxiliary_disposition']['accepted'], private and not changed)

    def test_expected_build_cleanup_does_not_complete_missing_product_proof(self):
        self.facts()
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            for phase in w.PHASES:
                if phase != 'inventory': w.write(phase + '.json', {'status': 'passed'})
            w.write('results.json', {})
            complete, phases = w.completion()
            self.assertFalse(complete)
            self.assertEqual(phases['inventory']['status'], 'missing')


class OwnedDescendantLifecycleSafety(unittest.TestCase):
    def test_successful_direct_exit_with_owned_child_still_refuses_and_retains_identity(self):
        events = []
        class Held:
            evidence = {'pid': 301, 'image_path': r'C:\tool\child.exe', 'creation_filetime': 123456,
                        'job_member': True, 'held_handle': True}
            def terminal(self):
                return 'terminate_owned_job' in events
            def close(self):
                events.append('close_held_handle')
        class Job:
            def capture_owned(self, direct_pid, known_pids=()):
                self.direct_pid = direct_pid
                return ([] if 301 in known_pids else [Held()]), []
            def active(self):
                return 0 if 'terminate_owned_job' in events else 1
            def terminate(self):
                events.append('terminate_owned_job')
        cleanup, unexpected, invalid = w.owned_exit_cleanup(Job(), 123, grace_seconds=0, cleanup_seconds=0)
        self.assertTrue(unexpected)  # Original successful-command/remaining-child guard stays strict.
        self.assertFalse(invalid)
        self.assertEqual(cleanup['active_after_direct_exit'], 1)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertEqual(cleanup['owned_descendants'][0]['pid'], 301)
        self.assertEqual(cleanup['owned_descendants'][0]['creation_filetime'], 123456)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertTrue(cleanup['owned_descendants'][0]['held_handle_closed'])
        self.assertEqual(events, ['terminate_owned_job', 'close_held_handle'])

    def test_unknown_and_uncontained_identity_refuse_even_when_owned_job_is_empty(self):
        for error in ('unknown image/creation identity', 'process no longer belongs to owned job'):
            with self.subTest(error=error):
                job = SimpleNamespace(capture_owned=lambda direct_pid, known_pids=(): ([], [{'pid': 301, 'error': error}]),
                                      active=lambda: 0, terminate=lambda: self.fail('empty job must not target any PID'))
                cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
                self.assertFalse(unexpected)
                self.assertTrue(invalid)
                self.assertEqual(cleanup['identity_errors'][0]['error'], error)
                self.assertEqual(cleanup['active_after_cleanup'], 0)

    def test_surviving_owned_process_or_unverified_held_terminal_refuses(self):
        class Held:
            evidence = {'pid': 301, 'job_member': True, 'held_handle': True}
            def terminal(self): return False
            def close(self): pass
        for remaining in (0, 1):
            with self.subTest(remaining=remaining):
                job = SimpleNamespace(capture_owned=lambda direct_pid, known_pids=(): ([Held()], []),
                                      active=lambda: remaining, terminate=lambda: None)
                cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
                self.assertTrue(invalid)
                self.assertFalse(cleanup['owned_descendants'][0]['terminal_after_cleanup'])

    def test_snapshot_and_held_handle_close_errors_are_not_success(self):
        def broken_snapshot(direct_pid, known_pids=()):
            raise OSError('identity query unavailable')
        job = SimpleNamespace(capture_owned=broken_snapshot, active=lambda: 0, terminate=lambda: None)
        cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
        self.assertTrue(invalid)
        self.assertEqual(cleanup['identity_errors'][0]['error'], 'identity query unavailable')
        class Held:
            evidence = {'pid': 301, 'job_member': True, 'held_handle': True}
            def terminal(self): return True
            def close(self): raise OSError('handle close failed')
        job.capture_owned = lambda direct_pid, known_pids=(): ([Held()], [])
        cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
        self.assertTrue(invalid)
        self.assertFalse(cleanup['owned_descendants'][0]['held_handle_closed'])
        self.assertEqual(cleanup['identity_errors'][0]['stage'], 'close_held_handle')

    def test_known_child_finishing_during_grace_retains_creation_and_reap_proof(self):
        events = []
        class Held:
            evidence = {'pid': 301, 'image_path': r'C:\tool\child.exe', 'creation_filetime': 123456,
                        'job_member': True, 'held_handle': True}
            def terminal(self): return True
            def close(self): events.append('closed')
        job = SimpleNamespace(capture_owned=lambda direct_pid, known_pids=(): ([Held()], []), active=lambda: 0,
                              terminate=lambda: self.fail('already completed child must not be killed'))
        cleanup, unexpected, invalid = w.owned_exit_cleanup(job, 123, grace_seconds=0, cleanup_seconds=0)
        self.assertFalse(unexpected)
        self.assertFalse(invalid)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertTrue(cleanup['owned_descendants'][0]['held_handle_closed'])
        self.assertEqual(events, ['closed'])


class OwnedDescendantReapSafety(unittest.TestCase):
    def exercise(self, signals, active_until=0, wait_error=None, close_error=None,
                 initially_active=True):
        clock, events = [0.0], []
        class Held:
            def __init__(self, index, signal_at):
                self.index, self.signal_at = index, signal_at
                self.evidence = {'pid': 301 + index, 'image_path': r'C:\tool\child.exe',
                                 'creation_filetime': 123456 + index,
                                 'job_member': True, 'held_handle': True}
            def terminal(self):
                if self.index == wait_error:
                    raise OSError('held process wait failed')
                return clock[0] >= self.signal_at
            def close(self):
                events.append(('close', self.index, clock[0]))
                if self.index == close_error:
                    raise OSError('held process close failed')
        class Job:
            terminated = False
            def capture_owned(self, direct_pid, known_pids=()):
                return [Held(i, signal_at) for i, signal_at in enumerate(signals)
                        if 301 + i not in known_pids], []
            def active(self):
                if not self.terminated:
                    return int(initially_active)
                return int(clock[0] < active_until)
            def terminate(self):
                self.terminated = True
                events.append(('terminate', clock[0]))
        def advance(seconds):
            self.assertGreater(seconds, 0)
            self.assertLessEqual(clock[0] + seconds, 1.000001)
            clock[0] += seconds
        with patch.object(w.time, 'monotonic', side_effect=lambda: clock[0]), \
             patch.object(w.time, 'sleep', side_effect=advance):
            result = w.owned_exit_cleanup(Job(), 123, grace_seconds=0, cleanup_seconds=1)
        return result, clock[0], events

    def test_empty_job_waits_for_all_held_process_signals_before_close(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise([0.2, 0.4])
        self.assertTrue(unexpected)  # A reaped lingering child still refuses command success.
        self.assertFalse(invalid)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertGreaterEqual(elapsed, 0.4)
        self.assertLess(elapsed, 1)
        self.assertTrue(all(r['terminal_after_cleanup'] and r['held_handle_closed']
                            for r in cleanup['owned_descendants']))
        self.assertEqual([e[:2] for e in events], [('terminate', 0.0), ('close', 0), ('close', 1)])
        self.assertTrue(all(e[2] >= 0.4 for e in events if e[0] == 'close'))

    def test_job_and_multiple_held_processes_share_one_cleanup_deadline(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise([0.6, 1.2], active_until=0.4)
        self.assertTrue(unexpected)
        self.assertTrue(invalid)
        self.assertAlmostEqual(elapsed, 1)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertFalse(cleanup['owned_descendants'][1]['terminal_after_cleanup'])
        self.assertTrue(all(r['held_handle_closed'] for r in cleanup['owned_descendants']))
        self.assertTrue(all(e[2] <= 1.000001 for e in events if e[0] == 'close'))

    def test_held_terminal_proof_is_required_even_without_job_termination(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise([0.2], initially_active=False)
        self.assertFalse(unexpected)
        self.assertFalse(invalid)
        self.assertGreaterEqual(elapsed, 0.2)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertEqual([e[:2] for e in events], [('close', 0)])

    def test_wait_and_close_failure_stay_failed_while_other_handles_are_reaped(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise(
            [0, 0.2], wait_error=0, close_error=0)
        self.assertTrue(unexpected)
        self.assertTrue(invalid)
        self.assertGreaterEqual(elapsed, 0.2)
        self.assertEqual(cleanup['active_after_cleanup'], 0)
        self.assertIsNone(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertFalse(cleanup['owned_descendants'][0]['held_handle_closed'])
        self.assertTrue(cleanup['owned_descendants'][1]['terminal_after_cleanup'])
        self.assertTrue(cleanup['owned_descendants'][1]['held_handle_closed'])
        self.assertEqual([e['stage'] for e in cleanup['identity_errors']],
                         ['held_process_reap', 'close_held_handle'])
        self.assertEqual([e[:2] for e in events if e[0] == 'close'], [('close', 0), ('close', 1)])

    def test_signaled_handles_do_not_replace_nonzero_job_count_refusal(self):
        (cleanup, unexpected, invalid), elapsed, events = self.exercise([0.2], active_until=2)
        self.assertTrue(unexpected)
        self.assertTrue(invalid)
        self.assertAlmostEqual(elapsed, 1)
        self.assertEqual(cleanup['active_after_cleanup'], 1)
        self.assertTrue(cleanup['owned_descendants'][0]['terminal_after_cleanup'])
        self.assertTrue(cleanup['owned_descendants'][0]['held_handle_closed'])


@unittest.skipUnless(os.name == 'nt', 'actual Windows process APIs unrun on non-Windows host')
class WindowsChildProcessRegression(unittest.TestCase):
    def test_native_suspension_prevents_execution_before_assignment(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'command-started.txt'
            command = [os.sys.executable, '-c', 'from pathlib import Path; Path(' + repr(str(marker)) + ').write_text("ran")']
            job = w.WindowsProcessJob()
            process = None
            try:
                process = w.WindowsSuspendedProcess(command)
                with self.assertRaises(subprocess.TimeoutExpired):
                    process.wait(timeout=0.1)
                self.assertFalse(marker.exists())
                w.assign_and_resume(job, process)
                self.assertEqual(process.wait(timeout=10), 0)
                self.assertEqual(marker.read_text(), 'ran')
                self.assertEqual(process.launch_order, ['created_suspended', 'assigned_to_job', 'resumed'])
            finally:
                job.terminate()
                if process is not None:
                    process.wait(timeout=10)
                    process.stdout.close()
                    process.close()
                job.close()

    def test_immediate_descendant_belongs_to_own_job_and_is_reaped(self):
        command = [os.sys.executable, '-c',
                   'import subprocess,sys,time; child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"]); '
                   'print(child.pid,flush=True); time.sleep(60)']
        job = w.WindowsProcessJob()
        process, child_handle, reader = None, None, None
        try:
            process = w.WindowsSuspendedProcess(command)
            w.assign_and_resume(job, process)
            pid_queue = queue.Queue()
            reader = threading.Thread(target=lambda: pid_queue.put(process.stdout.readline()), daemon=True)
            reader.start()
            child_pid = int(pid_queue.get(timeout=10))
            job.api.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
            job.api.OpenProcess.restype = ctypes.c_void_p
            child_handle = job.api.OpenProcess(0x100000 | 0x1000, False, child_pid)
            self.assertTrue(child_handle)
            job.api.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
            job.api.IsProcessInJob.restype = ctypes.c_int
            belongs = ctypes.c_int()
            self.assertTrue(job.api.IsProcessInJob(child_handle, job.handle, ctypes.byref(belongs)))
            self.assertTrue(belongs.value)
            self.assertGreaterEqual(job.active(), 2)
            job.terminate()
            process.wait(timeout=10)
            self.assertEqual(process.api.WaitForSingleObject(child_handle, 10000), 0)
            deadline = time.monotonic() + 5
            while job.active() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertEqual(job.active(), 0)
        finally:
            job.terminate()
            if process is not None:
                process.wait(timeout=10)
            if reader is not None:
                reader.join(timeout=2)
            if process is not None:
                process.stdout.close()
                process.close()
            if child_handle:
                job.api.CloseHandle(child_handle)
            job.close()

    def test_successful_parent_exit_retains_child_identity_and_still_refuses(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            marker = Path(tmp) / 'owned-child-pid.txt'
            script = ('import subprocess,sys; from pathlib import Path; '
                      'child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"], '
                      'stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); '
                      'Path(' + repr(str(marker)) + ').write_text(str(child.pid))')
            with self.assertRaisesRegex(RuntimeError, 'command failed'):
                w.measure('owned-child', [os.sys.executable, '-c', script], 10)
            record = json.loads((Path(tmp) / 'owned-child-command.json').read_text())
            self.assertEqual(record['exit'], 0)
            self.assertFalse(record['timed_out'])
            self.assertTrue(record['direct_terminal'])
            cleanup = record['cleanup']
            self.assertEqual(cleanup['active_after_direct_exit'], 1)
            self.assertEqual(cleanup['active_after_cleanup'], 0)
            self.assertFalse(cleanup['identity_errors'])
            self.assertEqual(len(cleanup['owned_descendants']), 1)
            identity = cleanup['owned_descendants'][0]
            self.assertEqual(identity['pid'], int(marker.read_text()))
            self.assertEqual(os.path.normcase(identity['image_path']), os.path.normcase(os.sys.executable))
            self.assertGreater(identity['creation_filetime'], 0)
            self.assertTrue(identity['job_member'])
            self.assertTrue(identity['held_handle'])
            self.assertTrue(identity['terminal_after_cleanup'])
            self.assertTrue(identity['held_handle_closed'])

    def test_generic_build_python_descendant_has_no_compiler_exception(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(w, 'EVIDENCE', Path(tmp)):
            script = ('import subprocess,sys; subprocess.Popen([sys.executable,"-c",'
                      '"import time; time.sleep(60)"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)')
            with self.assertRaisesRegex(RuntimeError, 'build command failed'):
                w.measure('build', [os.sys.executable, '-c', script], 10)
            record = json.loads((Path(tmp) / 'build-command.json').read_text())
            self.assertEqual(record['exit'], 0)
            self.assertTrue(record['unexpected_children'])
            self.assertFalse(record['auxiliary_disposition']['accepted'])
            self.assertEqual(record['cleanup']['active_after_cleanup'], 0)
            self.assertFalse(record['cleanup']['identity_errors'])
            child = record['cleanup']['owned_descendants'][0]
            self.assertTrue(child['job_member'])
            self.assertTrue(child['terminal_after_cleanup'])
            self.assertTrue(child['held_handle_closed'])

    def test_native_assignment_and_resume_failures_never_execute_command(self):
        for method, owner in (('attach', w.WindowsProcessJob), ('resume', w.WindowsSuspendedProcess)):
            with self.subTest(method=method), tempfile.TemporaryDirectory() as tmp:
                marker = Path(tmp) / 'should-not-run.txt'
                command = [os.sys.executable, '-c', 'from pathlib import Path; Path(' + repr(str(marker)) + ').write_text("unexpected")']
                with patch.object(w, 'EVIDENCE', Path(tmp)), patch.object(owner, method, side_effect=RuntimeError('injected ' + method + ' failure')):
                    with self.assertRaisesRegex(RuntimeError, 'injected ' + method + ' failure'):
                        w.measure('native-launch', command, 10)
                record = json.loads((Path(tmp) / 'native-launch-command.json').read_text())
                self.assertTrue(record['cleanup']['direct_terminal'])
                self.assertEqual(record['cleanup']['job_active_after_cleanup'], 0)
                self.assertNotIn('resumed', record['cleanup']['launch_order'])
                self.assertFalse(marker.exists())


class SDKScopeSafety(unittest.TestCase):
    def fixture(self, head=None, checkout_newline=None):
        def source(name):
            if head:
                raw = subprocess.check_output(['git', '-C', str(ROOT), 'show', head + ':' + name])
            else:
                raw = (ROOT/name).read_bytes()
            if checkout_newline is not None:
                raw = raw.replace(b'\r\n', b'\n').replace(b'\n', checkout_newline.encode())
            return raw
        # Checkout bytes may be CRLF; construct intentional test variants from LF.
        manifest = source('sdk/contract_headers.txt').decode().replace('\r\n', '\n')
        headers = manifest.splitlines()
        owners = source('sdk/header_owners.txt').decode().replace('\r\n', '\n')
        original = {h: hashlib.sha256(source('sdk/include/'+h)).hexdigest() for h in headers}
        objects = {'sdk/odbcpp_sdk_header_isolation.dir/Release/'+h.replace('/', '_')+'.obj':
                   {'bytes': 17, 'sha256': 'a'*64} for h in headers}
        return manifest, owners, original, dict(original), objects

    def test_original23_and_integrated24_have_exact_owners_staging_and_one_object_each(self):
        for head, count, digest in [(w.FIRST_PRODUCT, 23, '20c54a37f72e79d566b03eba312e6469e92217528065bc3b169b7d276e7f3bb4'),
                                    (None, 24, 'ede15fe5cb19bebf56a6cb02eb5080bd1505b83db94f087cba0fc46df85d342f')]:
            for checkout_newline in ['\n', '\r\n']:
                args = self.fixture(head, checkout_newline)
                for manifest in [args[0], args[0].replace('\n', '\r\n')]:
                    with self.subTest(count=count, checkout_newline=repr(checkout_newline)):
                        result = w.validate_sdk_inventory(manifest, *args[1:])
                        self.assertEqual(result['manifest_sha256'], digest)
                        self.assertEqual(len(result['headers']), count)
                        self.assertEqual(len(result['objects']), count)
                        self.assertEqual(result['owners'], {h: 'sdk/include/'+h for h in args[0].splitlines()})

    def test_count_preserving_unknown_duplicate_reordered_and_noncanonical_manifests_refuse(self):
        for head in [w.FIRST_PRODUCT, None]:
            manifest, *rest = self.fixture(head)
            headers = manifest.splitlines()
            variants = [headers[:-1], headers+['odbcpp/database/unknown.h'],
                        headers[:-1]+[headers[0]], ['odbcpp/database/unknown.h']+headers[1:],
                        list(reversed(headers)), headers[:1]+['']+headers[1:]]
            for lines in variants:
                with self.subTest(head=head, lines=lines), self.assertRaises(ValueError):
                    w.validate_sdk_inventory('\n'.join(lines)+'\n', *rest)
            with self.assertRaises(ValueError):
                w.validate_sdk_inventory(manifest.rstrip('\n'), *rest)
            with self.assertRaises(ValueError):
                w.validate_sdk_inventory(manifest.replace('\n', '\r\r\n'), *rest)

    def test_wrong_missing_duplicate_extra_owner_staging_or_object_binding_refuse(self):
        manifest, owners, original, staged, objects = self.fixture(checkout_newline='\r\n')
        header = manifest.splitlines()[0]
        row = header+'|sdk/include/'+header
        variants = [
            ('missing', owners.replace(row+'\n', '')),
            ('duplicate', owners+row+'\n'),
            ('wrong_owner', owners.replace(row, header+'|sdk/internal/'+header)),
            ('extra', owners+'odbcpp/database/unknown.h|sdk/include/odbcpp/database/unknown.h\n'),
        ]
        for mutation, changed in variants:
            with self.subTest(owner_mutation=mutation):
                self.assertNotEqual(changed, owners, 'negative owner mutation must change fixture bytes')
                if mutation in ('missing', 'wrong_owner'):
                    self.assertNotIn(row, changed.splitlines())
                elif mutation == 'duplicate':
                    self.assertEqual(changed.splitlines().count(row), 2)
                else:
                    self.assertIn('odbcpp/database/unknown.h|sdk/include/odbcpp/database/unknown.h', changed.splitlines())
                with self.assertRaises(ValueError):
                    w.validate_sdk_inventory(manifest, changed, original, staged, objects)
        for part in ['original', 'staged', 'objects']:
            for mutation in ['missing', 'extra', 'changed']:
                values = {'original': dict(original), 'staged': dict(staged), 'objects': deepcopy(objects)}
                key = next(iter(values[part]))
                if mutation == 'missing':
                    values[part].pop(key)
                elif mutation == 'extra':
                    values[part]['unknown'] = values[part][key]
                elif part == 'objects':
                    values[part][key]['bytes'] = 0
                else:
                    values[part][key] = 'b'*64
                with self.subTest(part=part, mutation=mutation), self.assertRaises(ValueError):
                    w.validate_sdk_inventory(manifest, owners, values['original'], values['staged'], values['objects'])
        alias = dict(objects)
        key = next(iter(alias))
        alias[key+'.alias'] = alias.pop(key)
        with self.assertRaises(ValueError):
            w.validate_sdk_inventory(manifest, owners, original, staged, alias)


class WorkflowSafety(unittest.TestCase):
    def test_reviewed_full_job_reference_refuses_unreviewed_body_changes(self):
        ci = (ROOT/'.github/workflows/ci.yml').read_text()
        self.assert_compatible_workflow(ci)
        for before, after in [("'it_redshift_routine_schema_patterns.cpp', ", ''),
                              ('if ($reports.Count -ne $expectedReports)', 'if ($reports.Count -lt $expectedReports)'),
                              ('--output-on-failure', '--output-on-failure --no-tests=ignore')]:
            self.assertIn(before, ci)
            with self.subTest(before=before), self.assertRaises(AssertionError):
                self.assert_compatible_workflow(ci.replace(before, after, 1))

    def test_sanitizer_defaultoff_and_windows_full_guards_cannot_be_removed(self):
        ci = (ROOT/'.github/workflows/ci.yml').read_text()
        block = ci.split('\n  windows-product-qualification:', 1)[1].split('\n  build-and-test:', 1)[0]
        self.assertIn('!inputs.run_sanitizer_only', block)
        with self.assertRaises(AssertionError):
            self.assert_compatible_workflow(ci.replace(block, block.replace(' && !inputs.run_sanitizer_only', '')))
        selector = ci.split('      run_sanitizer_only:', 1)[1].split('      run_development_pilot:', 1)[0]
        with self.assertRaises(AssertionError):
            self.assert_compatible_workflow(ci.replace(selector, selector.replace('default: false', 'default: true')))

    def assert_compatible_workflow(self, ci):
        # Reviewed qualification009 includes trace evidence, dual-product package
        # acceptance, static provider closure and exact producer exclusions.
        # FIRST_PRODUCT remains the unchanged numeric-product sentinel in the helper.
        reference = '3c267078b8e80198cc72cde1464c307406e63ea7'
        old = subprocess.check_output(['git', '-C', str(ROOT), 'show', reference + ':.github/workflows/ci.yml'], text=True)
        jobs = ('build-and-test', 'redshift-build', 'windows-live', 'windows-package',
                'windows-linked-server', 'linux-sanitizers', 'linux-openssl-static',
                'awslc-adapter-proof', 'iodbc-unicode')
        import re
        def job(text, name):
            block = re.search(r'^  ' + name + r':\n(.*?)(?=^  [a-z][a-z-]*:|\Z)', text, re.M | re.S)[1]
            return '\n'.join(line for line in block.splitlines() if not line.startswith('    if:'))
        for name in jobs:
            self.assertEqual(job(ci, name), job(old, name), name)
        self.assertIn('run_windows_product_qualification:\n', ci)
        window_input = ci.split('      run_windows_product_qualification:', 1)[1].split('      windows_product_targets:', 1)[0]
        self.assertIn('default: false', window_input)
        for name in jobs:
            guard = ci.split('\n  ' + name + ':', 1)[1].splitlines()[1]
            self.assertIn('run_windows_product_qualification', guard)
            self.assertIn('inputs.run_sanitizer_only', guard)
        def enabled(text, name, event, flags):
            block = re.search(r'^  ' + name + r':\n(.*?)(?=^  [a-z][a-z-]*:|\Z)', text, re.M | re.S)[1]
            expression = re.search(r'^    if: (.*)$', block, re.M)[1]
            expression = expression.replace('${{', '').replace('}}', '').replace('github.event_name', repr(event))
            expression = re.sub(r'inputs\.([a-z_]+)', lambda m: repr(flags[m[1]]), expression)
            expression = expression.replace('&&', ' and ').replace('||', ' or ')
            expression = re.sub(r'!(?!=)', ' not ', expression).strip()
            tree = ast.parse(expression, mode='eval')
            allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not,
                       ast.Compare, ast.Eq, ast.NotEq, ast.Constant, ast.Load)
            self.assertTrue(all(isinstance(n, allowed) for n in ast.walk(tree)))
            return bool(eval(compile(tree, '<literal workflow guard>', 'eval'), {'__builtins__': {}}, {}))
        modes = ['run_development_pilot', 'run_product_qualification',
                 'run_windows_product_qualification', 'run_linked_server']
        for event in ['pull_request', 'workflow_dispatch']:
            for values in itertools.product([False, True], repeat=4):
                flags = dict(zip(modes, values), run_sanitizer_only=False)
                for name in jobs:
                    if event == 'pull_request':
                        # PRs build on three platforms; full/live jobs remain checkpoint-only.
                        self.assertFalse(enabled(ci, name, event, flags), name)
                    else:
                        self.assertEqual(enabled(ci, name, event, flags), enabled(old, name, event, flags), name)
                        self.assertFalse(enabled(ci, name, event, dict(flags, run_sanitizer_only=True)), name)
                for name in ('pr-ubuntu-build', 'pr-windows-build'):
                    self.assertEqual(enabled(ci, name, event, flags), event == 'pull_request', name)
                    self.assertEqual(enabled(ci, name, event, dict(flags, run_sanitizer_only=True)),
                                     event == 'pull_request', name)
        sanitizer_input = ci.split('      run_sanitizer_only:', 1)[1].split('      run_development_pilot:', 1)[0]
        self.assertIn('default: false', sanitizer_input)
        windows_guard = ci.split('\n  windows-product-qualification:', 1)[1].splitlines()[2]
        self.assertIn('!inputs.run_sanitizer_only', windows_guard)
        self.assertIn("'windows-product-qualification'", ci)
        self.assertIn("'ubuntu-development-pilot'", ci)
        self.assertIn('cancel-in-progress: false', ci)
        self.assertIn('inputs.run_development_pilot || inputs.run_product_qualification || inputs.run_linked_server', ci)
        self.assertIn('inputs.run_product_qualification && !inputs.run_windows_product_qualification', ci)
        self.assertIn('inputs.run_development_pilot && !inputs.run_product_qualification && !inputs.run_windows_product_qualification', ci)

    def test_PR_build_platform_guard_cannot_be_dropped(self):
        ci = (ROOT/'.github/workflows/ci.yml').read_text()
        before = "if: github.event_name == 'pull_request'"
        self.assertIn(before, ci)
        with self.assertRaises(AssertionError):
            self.assert_compatible_workflow(ci.replace(before, "if: github.event_name == 'workflow_dispatch'", 1))

    def test_full_job_cannot_execute_on_PR_or_ignore_manual_conflicts(self):
        ci = (ROOT/'.github/workflows/ci.yml').read_text()
        block = ci.split('\n  build-and-test:', 1)[1].split('\n  redshift-build:', 1)[0]
        guard = block.splitlines()[1]
        # Preserve every original input guard while changing only event selection.
        before = "github.event_name == 'workflow_dispatch' && !("
        self.assertIn(before, guard)
        with self.assertRaises(AssertionError):
            self.assert_compatible_workflow(ci.replace(guard, guard.replace(before, "github.event_name != 'push' && !("), 1))
        before = 'inputs.run_product_qualification || '
        self.assertIn(before, guard)
        with self.assertRaises(AssertionError):
            self.assert_compatible_workflow(ci.replace(guard, guard.replace(before, ''), 1))

    def test_default_off_mutual_modes_and_full_body_preservation(self):
        self.assert_compatible_workflow((ROOT / '.github/workflows/ci.yml').read_text())

    def test_windows_job_has_no_restores_services_native_or_secret_steps(self):
        workflow = (ROOT / '.github/workflows/windows-product-qualification.yml').read_text()
        self.assertIn('runs-on: windows-2025', workflow)
        self.assertIn('timeout-minutes: 30', workflow)
        self.assertIn('retention-days: 3', workflow)
        self.assertIn('if: always()', workflow)
        for forbidden in ('actions/cache', 'services:', 'secrets:', 'windows-postgres', 'windows-system-dsn',
                          'packaging/windows', 'Start-Service', 'self-hosted', 'larger-runner'):
            self.assertNotIn(forbidden, workflow)
        self.assertIn('persist-credentials: false', workflow)
        self.assertIn('github.event.repository.private == false', workflow)
        self.assertIn("'--parallel', '2'", (ROOT / 'tools/ci/windows_product_qualification.py').read_text())
        self.assertIn('-DODBCPP_CRYPTO_PROFILE_LIVE_TESTS=OFF', w.PROFILE)
        self.assertIn('-DODBCPP_CRYPTO_TLS_PROOF=OFF', w.PROFILE)
        self.assertEqual(len(w.CHECKS), 6)
        self.assertEqual(len(w.NUMERIC_CASES), 4)


if __name__ == '__main__':
    unittest.main()
