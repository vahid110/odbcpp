"""Offline checks of pilot orchestration/evidence only; no product feature oracle."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('pilot', Path(__file__).with_name('ubuntu_development_pilot.py'))
pilot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pilot)


class PilotEvidenceTests(unittest.TestCase):
    def setUp(self):
        # Legacy-role checks also run inside the hosted normal job.
        env = patch.dict(pilot.os.environ, {"PILOT_MODE": "cold"})
        env.start()
        self.addCleanup(env.stop)

    def test_exact_ten_ctests_and_no_extra_native_inventory(self):
        listing = {'tests': [{'name': n} for n in pilot.SELECTED]}
        self.assertEqual(len(pilot.inventory_names(listing)), 10)
        for bad in (listing['tests'][:-1], listing['tests'] + [{'name': 'it_redshift_real'}],
                    listing['tests'][:-1] + [listing['tests'][0]]):
            with self.assertRaises(ValueError):
                pilot.inventory_names({'tests': bad})

    def test_google_test_actual_inventory_preserves_parameter_names(self):
        text = 'Running main() from gtest_main.cc\nSuite.\n  Alpha\nTyped/0.  # TypeParam = int\n  Beta\nInst/Params.\n  Gamma/0  # GetParam() = 2\n'
        self.assertEqual(pilot.gtest_names(text), ['Inst/Params.Gamma/0', 'Suite.Alpha', 'Typed/0.Beta'])
        for bad in ('', 'Suite.\n  DISABLED_Alpha\n', 'Suite.\n  Alpha\n  Alpha\n'):
            with self.assertRaises(ValueError):
                pilot.gtest_names(bad)

    def test_xml_requires_real_complete_success_no_skips_errors_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.xml'
            path.write_text('<testsuites><testsuite><testcase classname="Suite" name="Alpha" status="run" result="completed"/></testsuite></testsuites>')
            self.assertEqual(pilot.xml_result(path, ['Suite.Alpha'])['count'], 1)
            with self.assertRaises(ValueError):
                pilot.xml_result(path, ['Suite.Alpha', 'Suite.Missing'])
            for body in ('<failure/>', '<error/>', '<skipped/>'):
                path.write_text('<testsuites><testcase classname="Suite" name="Alpha">' + body + '</testcase></testsuites>')
                with self.assertRaises(ValueError):
                    pilot.xml_result(path, ['Suite.Alpha'])
            path.write_text('<testsuites><testcase classname="Suite" name="Alpha" status="notrun"/></testsuites>')
            with self.assertRaises(ValueError):
                pilot.xml_result(path, ['Suite.Alpha'])

    def test_reviewed_head_and_fixed_tracked_inputs(self):
        head = 'a' * 40
        with patch.object(pilot, 'output', side_effect=[head, '', 'tree']):
            self.assertEqual(pilot.source(head)['head'], head)
        for values, expected in (([head], 'b' * 40), ([head], 'branch'), ([head, ' M source.cpp'], head)):
            with patch.object(pilot, 'output', side_effect=values), self.assertRaises(ValueError):
                pilot.source(expected)

    def test_key_digest_binds_profile_dependency_and_source_inputs(self):
        original = {'source': 'a' * 40, 'dependency': '1.2', 'profile': list(pilot.PROFILE), 'runner': 'ubuntu24-x64'}
        for name in original:
            changed = copy.deepcopy(original)
            changed[name] = 'different'
            self.assertNotEqual(pilot.digest(original), pilot.digest(changed))
        self.assertEqual(pilot.digest(original), pilot.digest(dict(reversed(list(original.items())))))

    def test_real_child_failure_retains_original_log_exit_and_measurement(self):
        # Substitute only the GNU time executable on macOS; execute a real failing child.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake_time = root / 'fake-time.py'
            fake_time.write_text('import pathlib, subprocess, sys\npathlib.Path(sys.argv[3]).write_text("Maximum resident set size (kbytes): 123\\n")\nsys.exit(subprocess.call(sys.argv[4:]))\n')
            real_popen = subprocess.Popen
            def launch(cmd, **kwargs):
                return real_popen([sys.executable, str(fake_time), *cmd[1:]], **kwargs)
            with patch.object(pilot, 'EVIDENCE', root / 'evidence'), patch.object(pilot.subprocess, 'Popen', side_effect=launch):
                with self.assertRaises(RuntimeError):
                    pilot.measure('failed', [sys.executable, '-c', 'print("original failure"); raise SystemExit(7)'], 10)
                record = json.loads((pilot.EVIDENCE / 'failed.json').read_text())
                self.assertEqual(record['exit'], 7)
                self.assertEqual(record['gnu_time_max_child_rss_kib'], 123)
                self.assertIn('original failure', (pilot.EVIDENCE / 'failed.log').read_text())

    def test_dependency_install_and_identity_survive_both_write_orders(self):
        # Exercise the real CLI phase; replace apt and GNU time with a finite local child.
        for identity_first in (True, False):
            for exit_code in (0, 7):
                with self.subTest(identity_first=identity_first, exit_code=exit_code), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    fake_time = root / 'fake-time.py'
                    fake_time.write_text('import pathlib, subprocess, sys\npathlib.Path(sys.argv[3]).write_text("Maximum resident set size (kbytes): 123\\n")\nsys.exit(subprocess.call(sys.argv[4:]))\n')
                    real_popen = subprocess.Popen
                    def launch(cmd, **kwargs):
                        child = [sys.executable, '-c', f'print("dependency child log"); raise SystemExit({exit_code})']
                        return real_popen([sys.executable, str(fake_time), *cmd[1:4], *child], **kwargs)
                    identity = {'system_packages': 'preserved dependency identity', 'tools': {'cmake': 'test tool'}}
                    with patch.object(pilot, 'EVIDENCE', root / 'evidence'), patch.object(pilot.subprocess, 'Popen', side_effect=launch), patch.object(sys, 'argv', ['pilot', 'dependencies']):
                        if identity_first:
                            pilot.write('dependencies.json', identity)
                        if exit_code:
                            with self.assertRaises(RuntimeError):
                                pilot.main()
                        else:
                            pilot.main()
                        if not identity_first:
                            pilot.write('dependencies.json', identity)
                        self.assertEqual(json.loads((pilot.EVIDENCE / 'dependencies.json').read_text()), identity)
                        timing = json.loads((pilot.EVIDENCE / 'dependency-install.json').read_text())
                        self.assertEqual(timing['exit'], exit_code)
                        self.assertFalse(timing['timed_out'])
                        self.assertEqual(timing['gnu_time_max_child_rss_kib'], 123)
                        self.assertIn('Maximum resident set size', (pilot.EVIDENCE / 'dependency-install-time.txt').read_text())
                        self.assertIn('dependency child log', (pilot.EVIDENCE / 'dependency-install.log').read_text())

    def test_configure_preserves_timing_and_finish_indexes_all_phase_records(self):
        for configure_first in (True, False):
            with self.subTest(configure_first=configure_first), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                build, evidence = root / 'build', root / 'evidence'
                compiler = root / 'compiler'
                compiler.write_text('offline compiler identity fixture')
                identity = {'head': 'a' * 40, 'tree': 'b' * 40, 'mode': 'cold', 'submitted_at': '2026-01-01T00:00:00+00:00'}
                timing = {'exit': 0, 'elapsed_seconds': 1.25, 'gnu_time_max_child_rss_kib': 123}
                def record_phase(name):
                    pilot.write(name + '.json', timing)
                    (evidence / (name + '-time.txt')).write_text('raw exit0/time fixture')
                    (evidence / (name + '.log')).write_text('raw command log fixture')
                def configured_measure(name, command, timeout):
                    # Only the external configure command is stubbed; configure's writers run.
                    self.assertEqual(name, 'configure')
                    build.mkdir()
                    for filename in ('CMakeCache.txt', 'compile_commands.json', 'odbcpp-crypto-manifest.json'):
                        (build / filename).write_text('{}')
                    for dependency in ('googletest', 'spdlog'):
                        source = build / '_deps' / (dependency + '-src')
                        source.mkdir(parents=True)
                        (source / 'fixture.txt').write_text('offline dependency source')
                    record_phase(name)
                    return timing
                def tool_output(*command):
                    if command == ('ccache', '--print-stats'):
                        return 'cache_miss 0\ndirect_cache_hit 0'
                    return 'offline tool output'
                env = {'GITHUB_OUTPUT': str(root / 'output'), 'GITHUB_RUN_ID': '1', 'GITHUB_RUN_ATTEMPT': '1', 'CCACHE_DIR': str(root / 'cache')}
                with patch.object(pilot, 'BUILD', build), patch.object(pilot, 'EVIDENCE', evidence), patch.object(pilot, 'measure', side_effect=configured_measure), patch.object(pilot, 'output', side_effect=tool_output), patch.object(pilot.shutil, 'which', return_value=str(compiler)), patch.object(pilot, 'source', return_value=identity), patch.object(pilot.subprocess, 'run'), patch.dict(pilot.os.environ, env):
                    pilot.write('identity.json', identity)
                    if not configure_first:
                        record_phase('dependency-install')
                    pilot.configure()
                    if configure_first:
                        record_phase('dependency-install')
                    self.assertEqual(json.loads((evidence / 'dependency-install.json').read_text()), timing)
                    self.assertIn('fetched_source_trees', json.loads((evidence / 'dependencies.json').read_text()))
                    for name in ('build', 'tests'):
                        record_phase(name)
                    pilot.finish()
                    index = json.loads((evidence / 'evidence-index.json').read_text())
                    for name in ('dependency-install', 'configure', 'build', 'tests'):
                        for suffix in ('.json', '-time.txt', '.log'):
                            filename = name + suffix
                            self.assertIn(filename, index)
                            self.assertEqual(index[filename]['sha256'], pilot.sha(evidence / filename))
                    self.assertIn('dependencies.json', index)
                    self.assertIn('finish.json', index)
                    self.assertEqual(json.loads((evidence / 'dependency-install.json').read_text()), timing)

    def test_real_change_roles_require_exact_reviewed_product_pair(self):
        for role in pilot.REAL_ROLES:
            expected = pilot.SEED_HEAD if role == 'seed-a' else pilot.CHANGED_HEAD
            self.assertEqual(pilot.real_product_head(role, pilot.SEED_HEAD, pilot.CHANGED_HEAD), expected)
        for role, seed, changed in (('cold', pilot.SEED_HEAD, pilot.CHANGED_HEAD), ('cached-b', 'a' * 40, pilot.CHANGED_HEAD), ('cold-b', pilot.SEED_HEAD, 'b' * 40)):
            with self.assertRaises(ValueError):
                pilot.real_product_head(role, seed, changed)
        old_pairs = (
            ('9c0435d50bb02e0158c54d3abde9423f8d943541', '196f5b3facd565dfc9a2b767d9dda8107229f453'),
            ('d424bc4a2f7a2b75a18ed3e8497c048ac483167b', '3c441223f1dfecf9a9872faa5ea2781f8f925f4f'),
        )
        for role in pilot.REAL_ROLES:
            for old_seed, old_changed in old_pairs:
                for seed, changed in ((old_seed, old_changed), (old_seed, pilot.CHANGED_HEAD), (pilot.SEED_HEAD, old_changed)):
                    with self.subTest(role=role, seed=seed, changed=changed), self.assertRaises(ValueError):
                        pilot.real_product_head(role, seed, changed)

    def test_cache_compatibility_binds_actual_tools_flags_and_paths_separately(self):
        deps = {'compiler_hashes': {'g++': 'actual compiler bytes'}, 'fetched_source_trees': {'gtest': 'actual source digest'}, 'runner_image': 'ubuntu24'}
        commands = [{'command': 'g++ -O3 -c /product/core.cpp', 'directory': '/product/build', 'file': '/product/core.cpp'}]
        paths = {'product': '/product', 'build': '/product/build', 'workflow': '/workflow'}
        original = pilot.compatibility_digest(deps, commands, paths)
        for changed in ((dict(deps, compiler_hashes={'g++': 'different'}), commands, paths), (deps, [{'command': 'g++ -O0'}], paths), (deps, commands, dict(paths, build='/other'))):
            self.assertNotEqual(original, pilot.compatibility_digest(*changed))
        first = pilot.real_cache_key(pilot.SEED_HEAD, 'a' * 40, original, '123', '1')
        for values in ((pilot.SEED_HEAD, 'b' * 40, original, '123', '1'), (pilot.SEED_HEAD, 'a' * 40, original, '124', '1'), (pilot.SEED_HEAD, 'a' * 40, original, '123', '2')):
            self.assertNotEqual(first, pilot.real_cache_key(*values))
        with self.assertRaises(ValueError):
            pilot.real_cache_key(pilot.CHANGED_HEAD, 'a' * 40, original, '123', '1')
        with self.assertRaises(ValueError):
            pilot.real_cache_key('9c0435d50bb02e0158c54d3abde9423f8d943541', 'a' * 40, original, '123', '1')
        with self.assertRaises(ValueError):
            pilot.real_cache_key('d424bc4a2f7a2b75a18ed3e8497c048ac483167b', 'a' * 40, original, '123', '1')

    def test_selected_seed_key_rejects_compatibility_or_exact_identity_mismatch(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(pilot, 'EVIDENCE', Path(directory)):
            compatibility = 'c' * 64
            key = pilot.real_cache_key(pilot.SEED_HEAD, 'a' * 40, compatibility, '123', '1')
            env = {'PILOT_WORKFLOW_HEAD': 'a' * 40, 'PILOT_SEED_COMPATIBILITY': compatibility, 'PILOT_SEED_CACHE_KEY': key, 'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '1'}
            pilot.write('cache-key.json', {'compatibility_digest': compatibility})
            with patch.dict(pilot.os.environ, env):
                pilot.check_seed_cache()
                for name, value in (('PILOT_SEED_COMPATIBILITY', 'd' * 64), ('PILOT_SEED_CACHE_KEY', key + '-fallback'), ('GITHUB_RUN_ATTEMPT', '2')):
                    with patch.dict(pilot.os.environ, {name: value}), self.assertRaises(ValueError):
                        pilot.check_seed_cache()

    def test_empty_cache_refuses_preexisting_objects_or_graph_for_all_roles(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'compiler-cache'
            with patch.dict(pilot.os.environ, {'CCACHE_DIR': str(path)}):
                pilot.empty_cache()
                (path / 'existing-object').write_text('object')
                with self.assertRaises(ValueError):
                    pilot.empty_cache()

    def test_real_configure_separates_product_identity_from_seed_compatibility_key(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build, evidence = root / 'build', root / 'evidence'
            compiler = root / 'compiler'
            compiler.write_text('identical compiler bytes')
            def configured_measure(name, command, timeout):
                build.mkdir()
                (build / 'CMakeCache.txt').write_text('profile fixture')
                (build / 'compile_commands.json').write_text('[{"command":"g++ -O3 -c /product/input.cpp","directory":"/product/build","file":"/product/input.cpp"}]')
                (build / 'odbcpp-crypto-manifest.json').write_text('{}')
                for dependency in ('googletest', 'spdlog'):
                    source = build / '_deps' / (dependency + '-src')
                    source.mkdir(parents=True)
                    (source / 'fixture.txt').write_text('identical dependency source')
            env = {'GITHUB_OUTPUT': str(root / 'output'), 'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '1', 'PILOT_WORKFLOW_ROOT': str(root / 'workflow'), 'CCACHE_COMPILERCHECK': 'content'}
            records = []
            with patch.object(pilot, 'BUILD', build), patch.object(pilot, 'EVIDENCE', evidence), patch.object(pilot, 'measure', side_effect=configured_measure), patch.object(pilot, 'output', return_value='identical actual tool/config/package fixture'), patch.object(pilot.shutil, 'which', return_value=str(compiler)), patch.dict(pilot.os.environ, env):
                for role, head in (('seed-a', pilot.SEED_HEAD), ('cold-b', pilot.CHANGED_HEAD)):
                    pilot.write('identity.json', {'head': head, 'tree': head, 'mode': role, 'workflow_source': {'head': 'a' * 40}})
                    pilot.configure()
                    records.append(json.loads((evidence / 'cache-key.json').read_text()))
                    pilot.shutil.rmtree(build) # Only this temporary offline fixture.
            self.assertNotEqual(records[0]['product_head'], records[1]['product_head'])
            self.assertNotEqual(records[0]['profile_digest'], records[1]['profile_digest'])
            self.assertEqual(records[0]['compatibility_digest'], records[1]['compatibility_digest'])
            self.assertEqual(records[0]['key'], records[1]['key'])
            self.assertEqual(records[1]['seed_head'], pilot.SEED_HEAD)
            self.assertEqual(records[1]['workflow_head'], 'a' * 40)

    def test_cache_miss_and_zero_useful_hits_are_inconclusive(self):
        self.assertIn('inconclusive', pilot.cache_comparison('cached-b', '', 0))
        self.assertIn('zero useful', pilot.cache_comparison('cached-b', 'true', 0))
        self.assertIn('independent comparison pending', pilot.cache_comparison('cached-b', 'true', 80))
        self.assertNotIn('inconclusive', pilot.cache_comparison('cold-b', '', 0))

    def test_actual_inventory_delta_is_seven_cases_and_cached_b_equals_cold_b(self):
        seed = {'ctests': sorted(pilot.SELECTED), 'google_tests': {suite: [suite + '.Existing'] for suite in pilot.SUITES}}
        changed = copy.deepcopy(seed)
        for name in pilot.ADDED_CASES:
            suite = 'test_backend_contract' if name.startswith('BackendContractTest.') else 'test_numeric_scale_conversion' if name.startswith('NumericScaleApiTest.') else 'test_redshift_data_conversion'
            changed['google_tests'][suite].append(name)
        with tempfile.TemporaryDirectory() as directory, patch.object(pilot, 'EVIDENCE', Path(directory)):
            self.assertEqual(len(pilot.inventory_delta(seed, changed, pilot.digest(changed))['added']), 7)
            for bad in (dict(changed, ctests=[]), dict(changed, google_tests=seed['google_tests'])):
                with self.assertRaises(ValueError):
                    pilot.inventory_delta(seed, bad)
            moved = copy.deepcopy(changed)
            case = moved['google_tests']['test_backend_contract'].pop()
            moved['google_tests']['test_redshift_data_conversion'].append(case)
            with self.assertRaises(ValueError):
                pilot.inventory_delta(seed, moved)
            with self.assertRaises(ValueError):
                pilot.inventory_delta(seed, changed, 'wrong cold B digest')

    def test_init_binds_product_and_reviewed_workflow_checkouts_separately(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(pilot, 'EVIDENCE', Path(directory)):
            env = {'PILOT_SEED_HEAD': pilot.SEED_HEAD, 'PILOT_CHANGED_HEAD': pilot.CHANGED_HEAD, 'PILOT_WORKFLOW_HEAD': 'a' * 40, 'PILOT_WORKFLOW_ROOT': '/workflow', 'GITHUB_SHA': 'a' * 40, 'RUNNER_OS': 'Linux'}
            with patch.dict(pilot.os.environ, env), patch.object(pilot, 'source', side_effect=[{'head': pilot.CHANGED_HEAD, 'tree': 'product tree'}, {'head': 'a' * 40, 'tree': 'workflow tree'}]), patch.object(pilot, 'sha', return_value='c' * 64), patch.object(pilot, 'output', return_value='runner information'), patch.object(pilot.platform, 'machine', return_value='x86_64'), patch.object(Path, 'read_text', return_value='MemTotal: fixture'):
                pilot.init(pilot.CHANGED_HEAD, '2026-01-01T00:00:00Z', 'cold-b')
            identity = json.loads((pilot.EVIDENCE / 'identity.json').read_text())
            self.assertEqual(identity['head'], pilot.CHANGED_HEAD)
            self.assertEqual(identity['workflow_source']['head'], 'a' * 40)
            with patch.dict(pilot.os.environ, env), patch.object(pilot, 'source', return_value={'head': pilot.SEED_HEAD, 'tree': 'product tree'}), self.assertRaises(ValueError):
                pilot.init(pilot.SEED_HEAD, '2026-01-01T00:00:00Z', 'cold-b')

    def test_timeout_kills_owned_process_group_and_records_failed_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            process = unittest.mock.Mock(pid=12345)
            process.wait.side_effect = [subprocess.TimeoutExpired('child', 1), 0]
            with patch.object(pilot, 'EVIDENCE', Path(directory)), patch.object(pilot.subprocess, 'Popen', return_value=process), patch.object(pilot.os, 'killpg') as kill:
                with self.assertRaises(RuntimeError):
                    pilot.measure('timeout', ['child'], 1)
                kill.assert_called_once_with(12345, pilot.signal.SIGKILL)
                record = json.loads((Path(directory) / 'timeout.json').read_text())
                self.assertEqual(record['exit'], 124)
                self.assertTrue(record['timed_out'])

    def test_artifact_bound_and_exclusion_of_final_binaries(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(pilot, 'EVIDENCE', Path(directory)):
            root = Path(directory)
            (root / 'identity.json').write_text('{}')
            pilot.package()
            self.assertIn('identity.json', json.loads((root / 'evidence-index.json').read_text()))
            (root / 'final.so').write_bytes(b'binary')
            with self.assertRaises(ValueError):
                pilot.package()
            (root / 'final.so').unlink()
            with (root / 'oversized.log').open('wb') as f:
                f.seek(20 * 1024 * 1024)
                f.write(b'x')
            with self.assertRaises(ValueError):
                pilot.package()

class ProductQualificationTests(unittest.TestCase):
    def setUp(self):
        self.env = patch.dict(pilot.os.environ, {
            'PILOT_MODE': 'product', 'PILOT_PRODUCT_TARGETS': 'test_diagnostics_apis',
            'PILOT_PRODUCT_BASE': 'b' * 40, 'PILOT_PRODUCT_CACHE': '{}',
            'PILOT_WORKFLOW_HEAD': 'a' * 40, 'PILOT_WORKFLOW_ROOT': '/workflow',
            'PILOT_EXPERIMENT_REQUESTED': 'false', 'GITHUB_SHA': 'a' * 40,
        })
        self.env.start()
        self.addCleanup(self.env.stop)

    def producer_request(self):
        values = dict(source_head='c' * 40, workflow_head='a' * 40,
                      compatibility_digest='d' * 64, run_id='123', run_attempt='1')
        values['key'] = pilot.product_cache_key(values['source_head'], values['workflow_head'], values['compatibility_digest'], values['run_id'], values['run_attempt'])
        return values

    def test_literal_target_selection_and_actual_ctest_inventory(self):
        self.assertEqual(pilot.owning_suites(), ('test_diagnostics_apis',))
        names = pilot.owning_suites() + pilot.CHECKS
        self.assertEqual(len(pilot.inventory_names({'tests': [{'name': n} for n in names]})), 7)
        self.assertEqual(pilot.product_targets('test_backend_contract,test_sql_escape'), ('test_backend_contract', 'test_sql_escape'))
        for bad in ('', 'test_.*', 'test_diagnostics_apis;uname', ' test_diagnostics_apis',
                    'test_diagnostics_apis,test_diagnostics_apis', ','.join(pilot.PRODUCT_TARGETS), 'it_redshift_real'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                pilot.product_targets(bad)
        with self.assertRaises(ValueError):
            pilot.inventory_names({'tests': [{'name': n} for n in pilot.SELECTED]})

    def test_normal_init_separates_heads_and_rejects_invalid_role_base_conflict(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(pilot, 'EVIDENCE', Path(directory)):
            sources = [{'head': 'c' * 40, 'tree': 'd' * 40}, {'head': 'a' * 40, 'tree': 'e' * 40}]
            with patch.object(pilot, 'source', side_effect=copy.deepcopy(sources)), patch.object(pilot, 'sha', return_value='f' * 64), patch.object(pilot, 'output', return_value='host'), patch.object(pilot.platform, 'machine', return_value='x86_64'), patch.object(Path, 'read_text', return_value='RAM'), patch.dict(pilot.os.environ, {'RUNNER_OS': 'Linux'}):
                pilot.init('c' * 40, '2026-01-01T00:00:00Z', 'product')
            identity = json.loads((Path(directory) / 'identity.json').read_text())
            self.assertEqual(identity['head'], 'c' * 40)
            self.assertEqual(identity['workflow_source']['head'], 'a' * 40)
            self.assertEqual(identity['batch_base'], 'b' * 40)
            self.assertEqual(identity['selected'], ['test_diagnostics_apis', *pilot.CHECKS])
            for override in ({'PILOT_MODE': 'cached-b'}, {'PILOT_PRODUCT_BASE': 'branch'}, {'PILOT_EXPERIMENT_REQUESTED': 'true'}, {'GITHUB_SHA': 'f' * 40}):
                with patch.dict(pilot.os.environ, override), patch.object(pilot, 'source', side_effect=copy.deepcopy(sources)), self.assertRaises(ValueError):
                    pilot.init('c' * 40, '2026-01-01T00:00:00Z', 'product')
            with patch.object(pilot, 'source', return_value=sources[0]), self.assertRaises(ValueError):
                pilot.init('c' * 40, '2026-01-01T00:00:00Z', 'unknown')

    def test_after_guard_requires_both_immutable_clean_sources_even_after_failure(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(pilot, 'EVIDENCE', Path(directory)):
            pilot.write('identity.json', {'mode': 'product', 'head': 'c' * 40, 'workflow_source': {'head': 'a' * 40}})
            pilot.write('build.json', {'exit': 7})
            after=[{'head': 'c' * 40, 'tree': 'd' * 40}, {'head': 'a' * 40, 'tree': 'e' * 40}]
            with patch.object(pilot, 'source', side_effect=after) as guard:
                pilot.product_source_guard()
                self.assertEqual(guard.call_args_list, [unittest.mock.call('c' * 40), unittest.mock.call('a' * 40, Path('/workflow'))])
            self.assertEqual(json.loads((pilot.EVIDENCE / 'source-after.json').read_text())['product']['head'], 'c' * 40)
            with patch.object(pilot, 'source', side_effect=ValueError('tracked source changed')), self.assertRaises(ValueError):
                pilot.product_source_guard()
            self.assertEqual(json.loads((pilot.EVIDENCE / 'build.json').read_text())['exit'], 7)

    def test_preflight_checks_exact_changed_source_before_any_product_commands(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(pilot, 'EVIDENCE', Path(directory)):
            pilot.write('identity.json', {'head': 'c' * 40, 'batch_base': 'b' * 40, 'mode': 'product'})
            with patch.object(pilot, 'output', side_effect=['b' * 40, 'tests/unit/test_diagnostics_apis.cpp\nREADME.md']), patch.object(pilot, 'sha', return_value='a' * 64), patch.object(pilot.subprocess, 'run') as git, patch.object(pilot, 'measure') as measure:
                pilot.product_preflight()
                git.assert_called_once_with(['git', 'merge-base', '--is-ancestor', 'b' * 40, 'c' * 40], check=True, timeout=30)
                measure.assert_called_once_with('source-preflight', ['python3', '-B', '/workflow/tools/development/check_changed_gtest_control_flow.py', '--base', 'b' * 40, '--head', 'c' * 40, 'tests/unit/test_diagnostics_apis.cpp'], 120)
            with patch.object(pilot, 'output', side_effect=['b' * 40, 'README.md']), patch.object(pilot.subprocess, 'run'), patch.object(pilot, 'measure') as measure, self.assertRaises(ValueError):
                pilot.product_preflight()
            measure.assert_not_called()

    def test_exact_producer_cache_identity_and_independent_compatibility_rejection(self):
        request = self.producer_request()
        self.assertEqual(pilot.product_cache_request(json.dumps(request)), request)
        for bad in (dict(request, key=request['key'] + '-fallback'), dict(request, source_head='main'),
                    dict(request, workflow_head='branch'), dict(request, run_attempt='0'),
                    dict(request, run_id=123), dict(request, extra='fallback'), []):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                pilot.product_cache_request(json.dumps(bad))
        with tempfile.TemporaryDirectory() as directory, patch.object(pilot, 'EVIDENCE', Path(directory)), patch.dict(pilot.os.environ, {'GITHUB_OUTPUT': directory + '/out'}):
            pilot.write('cache-key.json', {'compatibility_digest': 'd' * 64})
            pilot.check_product_cache()
            self.assertIn('restore=false\nrestore_key=\n', (Path(directory) / 'out').read_text())
            with patch.dict(pilot.os.environ, {'PILOT_PRODUCT_CACHE': json.dumps(request)}):
                pilot.check_product_cache()
                self.assertIn('restore_key=' + request['key'], (Path(directory) / 'out').read_text())
                pilot.write('cache-key.json', {'compatibility_digest': 'e' * 64})
                with self.assertRaises(ValueError):
                    pilot.check_product_cache()

    def test_restore_requires_exact_successful_embedded_owning_producer(self):
        request = self.producer_request()
        producer = {'identity': request, 'role': 'product', 'source_tree': 'e' * 40, 'workflow_tree': 'f' * 40,
                    'phase_exits': {p: 0 for p in ('source-preflight', 'dependency-install', 'configure', 'build', 'tests')},
                    'targets': ['test_diagnostics_apis'], 'counts': {'test_diagnostics_apis': 3}, 'inventory_digest': '1' * 64}
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / 'cache'; cache.mkdir()
            record = cache / 'odbcpp-product-producer.json'
            with patch.object(pilot, 'EVIDENCE', Path(directory) / 'evidence'), patch.dict(pilot.os.environ, {'CCACHE_DIR': str(cache), 'PILOT_CACHE_HIT': 'true', 'PILOT_RESTORED_KEY': request['key']}):
                pilot.write('selected-product-cache.json', {'requested': True, 'producer': request})
                record.write_text(json.dumps(producer))
                pilot.check_product_restore()
                self.assertEqual(json.loads((pilot.EVIDENCE / 'product-cache-restore.json').read_text())['producer_record_sha256'], pilot.sha(record))
                for bad in (dict(producer, role='seed-a'), dict(producer, identity=dict(request, run_attempt='2')),
                            dict(producer, phase_exits=dict(producer['phase_exits'], tests=7)),
                            dict(producer, phase_exits=dict(producer['phase_exits'], tests=False)),
                            dict(producer, counts={'test_diagnostics_apis': 0}), dict(producer, source_tree='unknown'),
                            dict(producer, inventory_digest='unknown'), dict(producer, targets=['it_redshift_real'])):
                    record.write_text(json.dumps(bad))
                    with self.subTest(bad=bad), self.assertRaises(ValueError):
                        pilot.check_product_restore()
                record.write_text(json.dumps(producer))
                with patch.dict(pilot.os.environ, {'PILOT_RESTORED_KEY': request['key'] + '-fallback'}), self.assertRaises(ValueError):
                    pilot.check_product_restore()
                record.unlink()
                with patch.dict(pilot.os.environ, {'PILOT_CACHE_HIT': ''}):
                    pilot.check_product_restore()
                    self.assertFalse(json.loads((pilot.EVIDENCE / 'product-cache-restore.json').read_text())['hit'])

    def test_normal_test_phase_refuses_changed_binary_and_retains_measured_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build=root / 'build'; evidence=root / 'evidence'
            (build / 'tests').mkdir(parents=True)
            binary = build / 'tests/test_diagnostics_apis'; binary.write_text('original executable')
            with patch.object(pilot, 'BUILD', build), patch.object(pilot, 'EVIDENCE', evidence):
                pilot.write('executables.json', {'test_diagnostics_apis': {'sha256': pilot.sha(binary), 'names': ['Suite.Case']}})
                def run_tests(name, command, timeout):
                    self.assertIn('--no-tests=error', command)
                    self.assertIn(pilot.selected_regex(), command)
                    pilot.write('tests.json', {'exit': 0})
                    binary.write_text('unexpected changed executable')
                with patch.object(pilot, 'measure', side_effect=run_tests), self.assertRaisesRegex(ValueError, 'executable changed'):
                    pilot.test()
                self.assertEqual(json.loads((evidence / 'tests.json').read_text())['exit'], 0)
                with patch.object(pilot, 'main', side_effect=RuntimeError('original exit7')), patch.object(sys, 'argv', ['pilot', 'test']), self.assertRaisesRegex(RuntimeError, 'original exit7'):
                    pilot.run_cli()
                failure = json.loads((evidence / 'product-failure-test.json').read_text())
                self.assertEqual(failure['error'], 'original exit7')
                self.assertEqual(json.loads((evidence / 'tests.json').read_text())['exit'], 0)

    def test_real_normal_configure_finish_preserve_phases_and_only_save_complete_results(self):
        for disposition in ('complete', 'failed', 'missing'):
            with self.subTest(disposition=disposition), tempfile.TemporaryDirectory() as directory:
                root=Path(directory); build=root / 'build'; evidence=root / 'evidence'; cache=root / 'cache'; cache.mkdir()
                compiler=root / 'compiler'; compiler.write_text('offline compiler fixture')
                identity={'head': 'c' * 40, 'tree': 'e' * 40, 'mode': 'product', 'submitted_at': '2026-01-01T00:00:00+00:00', 'workflow_source': {'head': 'a' * 40, 'tree': 'f' * 40}}
                def measure(name, command, timeout):
                    build.mkdir()
                    for name2 in ('CMakeCache.txt', 'odbcpp-crypto-manifest.json'): (build / name2).write_text('{}')
                    (build / 'compile_commands.json').write_text('[]')
                    for dep in ('googletest', 'spdlog'):
                        src=build / '_deps' / (dep + '-src'); src.mkdir(parents=True); (src / 'file').write_text('source')
                    pilot.write(name + '.json', {'exit': 0})
                def output(*cmd):
                    return 'direct_cache_hit 0\ncache_miss 1' if cmd == ('ccache', '--print-stats') else 'offline fixture'
                env={'CCACHE_DIR': str(cache), 'GITHUB_OUTPUT': str(root / 'out'), 'GITHUB_RUN_ID': '123', 'GITHUB_RUN_ATTEMPT': '1', 'PILOT_CACHE_HIT': ''}
                with patch.object(pilot, 'BUILD', build), patch.object(pilot, 'EVIDENCE', evidence), patch.object(pilot, 'measure', side_effect=measure), patch.object(pilot, 'output', side_effect=output), patch.object(pilot.shutil, 'which', return_value=str(compiler)), patch.object(pilot, 'source', return_value=identity), patch.object(pilot.subprocess, 'run'), patch.dict(pilot.os.environ, env):
                    pilot.write('identity.json', identity)
                    pilot.write('dependency-install.json', {'exit': 0, 'elapsed_seconds': 1.25})
                    (evidence / 'dependency-install-time.txt').write_text('raw GNU time')
                    (evidence / 'dependency-install.log').write_text('original child output')
                    pilot.configure()
                    self.assertEqual(json.loads((evidence / 'dependency-install.json').read_text())['elapsed_seconds'], 1.25)
                    for name in ('source-preflight', 'build', 'tests'): pilot.write(name + '.json', {'exit': 7 if name == 'build' and disposition == 'failed' else 0})
                    if disposition == 'missing': (evidence / 'source-preflight.json').unlink()
                    pilot.write('results.json', {'test_diagnostics_apis': {'count': 3, 'failures': 0, 'errors': 0, 'skips': 0, 'disabled_or_notrun': 0}})
                    pilot.write('semantic-inventory.json', {'ctests': list(pilot.selected_tests()), 'google_tests': {'test_diagnostics_apis': ['Suite.One', 'Suite.Two', 'Suite.Three']}})
                    pilot.check_product_cache()
                    pilot.finish()
                    saved=cache / 'odbcpp-product-producer.json'
                    self.assertEqual(saved.exists(), disposition == 'complete')
                    self.assertEqual(json.loads((evidence / 'product-cache-eligibility.json').read_text())['save'], disposition == 'complete')
                    index=json.loads((evidence / 'evidence-index.json').read_text())
                    for name in ('dependencies.json', 'dependency-install.json', 'dependency-install-time.txt', 'dependency-install.log', 'configure.json', 'build.json', 'tests.json', 'finish.json'):
                        self.assertEqual(index[name]['sha256'], pilot.sha(evidence / name))
                    if disposition == 'complete':
                        producer=json.loads(saved.read_text())
                        self.assertEqual(producer['counts'], {'test_diagnostics_apis': 3})
                        self.assertEqual(producer['identity']['workflow_head'], 'a' * 40)
                        self.assertEqual(producer['identity']['source_head'], 'c' * 40)
                        self.assertEqual(set(producer['phase_exits']), {'source-preflight', 'dependency-install', 'configure', 'build', 'tests'})
                        self.assertIn('save_cache=true', (root / 'out').read_text())
                    else:
                        self.assertIn('save_cache=false', (root / 'out').read_text())

    def test_manual_normal_mode_default_off_and_bounded_single_job(self):
        root = Path(__file__).resolve().parents[2]
        caller=(root / '.github/workflows/ci.yml').read_text()
        reusable=(root / '.github/workflows/ubuntu-development-pilot.yml').read_text()
        input_block=caller.split('      run_product_qualification:', 1)[1].split('      product_workflow_head:', 1)[0]
        self.assertIn('type: boolean', input_block)
        self.assertIn('default: false', input_block)
        self.assertIn('cancel-in-progress: false', caller)
        self.assertIn("(inputs.run_development_pilot || inputs.run_product_qualification || inputs.run_sanitizer_only)", caller)
        product_job=reusable.split('  product:\n', 1)[1].split('  cold:\n', 1)[0]
        self.assertEqual(product_job.count('runs-on:'), 1)
        self.assertIn('runs-on: ubuntu-24.04', product_job)
        self.assertIn('timeout-minutes: 30', product_job)
        self.assertIn('retention-days: 3', product_job)
        self.assertNotIn('restore-keys:', product_job)
        self.assertNotIn('secrets:', product_job)
        self.assertLess(product_job.index(' product-preflight'), product_job.index(' dependencies'))
        self.assertLess(product_job.index(' product-restore-check'), product_job.index(' build'))


if __name__ == '__main__':
    unittest.main()
