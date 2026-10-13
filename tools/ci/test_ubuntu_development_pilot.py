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


if __name__ == '__main__':
    unittest.main()
