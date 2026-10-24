"""Cloud-free normal CLI preparation/preflight and authority-boundary checks."""
import hashlib
import copy
import datetime as dt
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.development import live_checkpoint as checkpoint
from tools.development import redshift_fixture_factory as factory
from tools.development import redshift_profile_inputs as profile_inputs
from tools.development.redshift_ctest_inputs import LoggingFactory


class CheckpointTests(unittest.TestCase):
    def test_composed_entry_binds_real_profile_driver_before_private_inputs_and_retires_dummy_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            driver = root / 'dummy-driver.so'; driver.write_bytes(b'OFFLINE_DUMMY_DRIVER')
            review = root / 'review.json'; review.write_text('{}')
            ca = root / 'ca.pem'; ca.write_bytes(b'OFFLINE_DUMMY_CA')
            pin = lambda path: dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            profile = dict(profile_name='offline-own-dsn', limits=dict(factory.CANONICAL_LIMITS),
                connection_environment=['ODBCPP_TEST_DSN'], uses_private_dsn=True, variables={},
                review=pin(review), private_input=dict(kind='OWN_DSN', password_rejection=False,
                    driver=dict(**pin(driver), wchar_bytes=2, kind='OWN')))
            profiles = {'own': profile}
            dummy = dict(host='offline.invalid', port=5439, database='odbcpp_pilot',
                admin_user='odbcpp_pilot_admin', admin_password='OFFLINE_DUMMY',
                test_user='odbcpp_pilot_test', test_password='OFFLINE_DUMMY', ca=str(ca))
            # Reproduce the actual missing binding with real preparation, then
            # close its positively identified empty residue. No fake exception.
            with self.assertRaises(profile_inputs.ProfileInputBlocked) as failure:
                profile_inputs.prepare_profile_inputs(root / 'old-inputs', dummy,
                    LoggingFactory(factory), profiles, {'pins': {}})
            self.assertTrue(profile_inputs.close_profile_inputs(failure.exception.residue))
            self.assertFalse((root / 'old-inputs').exists())
            plan_file = root / 'plan.json'; plan_file.write_text('{}')
            plan = dict(source_head='a' * 40, run_name='offline026', state_directory=str(root),
                resource={'kind': 'serverless'}, timeout_fixture=dict(table='odbcpp_timeout_gate_offline026',
                    expected_principal=None, allow_current_identity=True, resource_kind='serverless'),
                ca_sha256=pin(ca)['sha256'], security_groups=[], authority_receipts={})
            executable = Path(sys.executable).resolve(strict=True)
            plan['tools'] = {key: pin(executable) for key in ('cmake', 'ctest', 'psql', 'aws', 'python')}
            for key, value in (('profiles', profiles), ('specs', []), ('index', {}), ('expected', {}), ('ci_receipt', {})):
                path = root / (key + '.json'); path.write_text(json.dumps(value)); plan[key] = pin(path)
            admission = dict(reviewed=True, plan_sha256=pin(plan_file)['sha256'], source_head=plan['source_head'],
                inventory_scope='complete_applicable', run_name=plan['run_name'], nonce='offline_nonce_026',
                bounds={}, expires_utc=(dt.datetime.now(dt.timezone.utc) + dt.timedelta(days=1)).isoformat(),
                credential_source={}, database_config={})
            def run(config, ops, fixtures):
                self.assertEqual(config.public_pins[str(driver)], pin(driver)['sha256'])
                self.assertEqual(ops.profile_manifest['pins'][str(driver)], pin(driver)['sha256'])
                prepared = profile_inputs.prepare_profile_inputs(root / 'new-inputs', dummy,
                    LoggingFactory(factory), ops.profiles, ops.profile_manifest)
                self.assertIsNotNone(prepared['profiles']['own']['driver'])
                self.assertTrue(profile_inputs.close_profile_inputs(prepared))
                self.assertFalse((root / 'new-inputs').exists())
                return {'offline_composition': True}
            with patch.object(checkpoint.subprocess, 'check_output', side_effect=['a' * 40, '']), \
                 patch.object(checkpoint, 'read_plan', return_value=plan), \
                 patch('tools.development.redshift_resource.private_json', return_value=admission) as private, \
                 patch.object(checkpoint.lifecycle, 'run_once', side_effect=run):
                self.assertEqual(checkpoint.execute(plan, plan_file, root / 'not-read'), {'offline_composition': True})
                private.assert_called_once()
            self.assertEqual(checkpoint.profile_driver_pins(profiles, {str(driver): pin(driver)['sha256']}),
                             {str(driver): pin(driver)['sha256']})
            for changed in ({str(driver): '0' * 64},):
                with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'profile_driver_pin_collision'):
                    checkpoint.profile_driver_pins(profiles, changed)
            conflicting = copy.deepcopy(profiles)
            conflicting['second'] = copy.deepcopy(profile)
            conflicting['second']['private_input']['driver']['sha256'] = '0' * 64
            with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'profile_driver_pin_collision'):
                checkpoint.profile_driver_pins(conflicting, {})
            driver.write_bytes(b'CHANGED_OFFLINE_DRIVER')
            with patch.object(checkpoint.subprocess, 'check_output', side_effect=['a' * 40, '']), \
                 patch.object(checkpoint, 'read_plan', return_value=plan), \
                 patch('tools.development.redshift_resource.private_json') as private:
                with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'public_binding_changed'):
                    checkpoint.execute(plan, plan_file, root / 'must-not-read')
                private.assert_not_called()

    def test_native_execution_envelope_refuses_short_before_resource_acquisition(self):
        for execution in (1200, 1559):
            with self.subTest(execution=execution), self.assertRaisesRegex(checkpoint.lifecycle.Refused,
                    'native_complete_suite_execution_envelope'):
                checkpoint.validate_execution_bounds({'native_windows': {}}, checkpoint.lifecycle.Bounds(execution=execution))
        checkpoint.validate_execution_bounds({'native_windows': {}}, checkpoint.lifecycle.Bounds(execution=1800))
        checkpoint.validate_execution_bounds({}, checkpoint.lifecycle.Bounds(execution=1200))

    def test_prepare_and_real_preflight_never_open_protected_inputs(self):
        os.umask(0o077)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root/'synthetic-image';image.write_bytes(b'not executable')
            case = dict(id='synthetic',image='owner',profile='test',
                        gtest_filter='Probe.Case',timeout_seconds=40)
            index = dict(schema_version=1,images={'owner':dict(executable=str(image),
                sha256=hashlib.sha256(image.read_bytes()).hexdigest())},
                profiles={'test':{'environment_file':str(root/'private-input-must-not-be-read')}},
                cases=[case])
            records = {'index':index,'expected':dict(schema_version=1,cases=[
                {k:v for k,v in case.items() if k!='timeout_seconds'}]),'profiles':{},'specs':[]}
            for name, value in records.items():
                (root/(name+'.json')).write_text(json.dumps(value))
            authority = {}
            for key in ('authority','applicability','cleanup_delegation'):
                path=root/(key+'.json');path.write_text('{"synthetic_offline_only":true}')
                authority[key]=str(path)
            recipe = {key:str(root/(key+'.json')) for key in records}
            recipe.update(state_directory=str(root),run_name='synthetic-only',
                synthetic_unicode_marker='odbcpp_u_\u00e9\u8868',
                tools={key:shutil.which(key) if key in ('cmake','ctest') else sys.executable
                       for key in ('cmake','ctest','aws','psql','python')},
                authority_receipts=authority,ca_sha256='0'*64,
                resource=dict(account='385207570137',
                    arn='arn:aws:iam::385207570137:user/odbcpp-redshift-pilot',
                    cluster='odbcpp-redshift-provisioned-pilot',created='2026-10-03T13:29:23.907Z',
                    region='eu-north-1',profile='odbcpp-redshift-pilot',node_type='synthetic'))
            recipe_path=root/'recipe.json';recipe_path.write_text(json.dumps(recipe,ensure_ascii=False),encoding='utf-8')
            plan_path=root/'plan.json'
            checkpoint.prepare_plan(recipe_path,plan_path)
            plan=checkpoint.read_plan(plan_path)
            self.assertEqual(plan['source_directory'],str(checkpoint.ROOT))
            self.assertEqual(plan['synthetic_unicode_marker'],'odbcpp_u_\u00e9\u8868')
            self.assertNotIn('credential_source',plan)
            from tools.development.test_redshift_serverless_resource import resource
            serverless=dict(plan,resource=resource(),timeout_fixture=dict(
                table='odbcpp_timeout_gate_synthetic',expected_principal=None,
                allow_current_identity=True,resource_kind='serverless'))
            serverless_path=root/'serverless-plan.json'
            serverless_path.write_text(json.dumps(serverless))
            self.assertEqual(checkpoint.read_plan(serverless_path)['resource']['kind'],'serverless')
            for timeout in ({},dict(serverless['timeout_fixture'],expected_principal={'owner_oid':101}),
                            dict(serverless['timeout_fixture'],allow_current_identity=False)):
                serverless['timeout_fixture']=timeout
                serverless_path.write_text(json.dumps(serverless))
                with self.assertRaisesRegex(checkpoint.lifecycle.Refused,
                                            'serverless_current_principal_discovery_required'):
                    checkpoint.read_plan(serverless_path)
            wrong_region=json.loads(plan_path.read_text())
            wrong_region['resource']['region']='eu-central-1'
            wrong_path=root/'wrong-region.json';wrong_path.write_text(json.dumps(wrong_region))
            with self.assertRaisesRegex(checkpoint.lifecycle.Refused,'approved_region_eu_north_1'):
                checkpoint.read_plan(wrong_path)
            with self.assertRaises(FileExistsError):
                checkpoint.prepare_plan(recipe_path,plan_path)
            with self.assertRaises(checkpoint.lifecycle.Refused):
                checkpoint.read_plan(plan_path,require_ci=True)
            # A finite partition must retain a pinned, complete checkpoint
            # union. It cannot re-label this smaller index as full coverage.
            from tools.development.redshift_powerbi_partition import CASES
            full_cases = records['expected']['cases'] + [
                dict(id='relationship'+str(i), image='owner', profile='test',
                     gtest_filter=name) for i, name in enumerate(CASES)]
            full_path = root/'checkpoint-expected.json'
            full_path.write_text(json.dumps(dict(schema_version=1,cases=full_cases)))
            partitions_path = root/'checkpoint-partitions.json'
            partitions_path.write_text(json.dumps(dict(schema_version=1,
                source_head=plan['source_head'],partitions={
                    'native':['synthetic'],
                    'powerbi_relationship':['relationship0','relationship1']})))
            partition_recipe = dict(recipe, partition='native',
                checkpoint_expected=str(full_path),
                checkpoint_partitions=str(partitions_path))
            partition_recipe_path=root/'partition-recipe.json'
            partition_recipe_path.write_text(json.dumps(partition_recipe))
            partition_plan_path=root/'partition-plan.json'
            checkpoint.prepare_plan(partition_recipe_path,partition_plan_path)
            partition_plan=checkpoint.read_plan(partition_plan_path)
            self.assertIn('sha256',partition_plan['checkpoint_expected'])
            missing=json.loads(partition_plan_path.read_text())
            missing.pop('checkpoint_partitions')
            missing_path=root/'missing-partition.json'
            missing_path.write_text(json.dumps(missing))
            with self.assertRaisesRegex(checkpoint.lifecycle.Refused,
                                        'checkpoint_partition_bindings'):
                checkpoint.read_plan(missing_path)
            full_path.write_text('{"cases":[]}')
            with self.assertRaisesRegex(checkpoint.lifecycle.Refused,
                                        'public_binding_changed'):
                checkpoint.read_plan(partition_plan_path)
            result=checkpoint.preflight(checkpoint.pinned(plan['index']),
                checkpoint.pinned(plan['expected']),root/'preflight',
                str(checkpoint.pinned(plan['tools']['cmake'])))
            self.assertEqual(result['configure_exit'],0)
            self.assertFalse(result['private_inputs_read'])
            self.assertFalse(result['live_admission_granted'])
            self.assertEqual(result['resource_operations'],0)
            image.write_bytes(b'changed')
            self.assertNotEqual(checkpoint.preflight(checkpoint.pinned(plan['index']),
                checkpoint.pinned(plan['expected']),root/'changed-image',
                str(checkpoint.pinned(plan['tools']['cmake'])))['configure_exit'],0)

    def test_native_receive_component_has_no_historical_code_imports(self):
        source=(checkpoint.ROOT/'tools/development/windows_live_backend.py').read_text()
        self.assertNotIn('importlib',source)
        self.assertNotIn('load_module',source)
        self.assertNotIn('/private/tmp/',source)
        self.assertNotIn('protected-lifecycle-extension',source)


class LocalValidationTests(unittest.TestCase):
    """Pinned synthetic coordinator assessments; no product/live acceptance."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.head = subprocess.check_output(['git', '-C', str(checkpoint.ROOT),
            'rev-parse', 'HEAD'], text=True).strip()
        self.evidence = self.write('raw-evidence', {'synthetic_offline_only': True})
        self.direction = self.write('direction', {'human_direction': 'SYNTHETIC Windows first, then WSL'})
        self.plan = dict(source_head=self.head, native_windows={}, validation_profile='windows-redshift',
            index={'sha256': '1'*64}, profiles={'sha256': '2'*64})
        self.receipt = dict(schema_version=1, kind='human-directed-local-validation-v1',
            reviewed=True, status='completed', human_direction='windows-first-then-wsl',
            human_direction_ref=self.direction, complete_applicable_offline=True,
            ci_acceptance=False, ci_status='pending_not_accepted', source_head=self.head,
            producer_head=self.head, profile='windows-redshift', build_exit=0, ctest_exit=0,
            evidence=self.evidence, runtime_index_sha256='1'*64, profiles_sha256='2'*64)
        self.refresh()

    def write(self, name, value):
        path = self.root / (name + '.json'); path.write_text(json.dumps(value))
        return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())

    def refresh(self):
        self.plan['local_validation'] = self.write('receipt', self.receipt)

    def validate(self):
        checkpoint.validate_local_validation(self.plan)

    def test_pinned_coordinator_assessment_preserves_CI_pending(self):
        self.validate()
        self.assertIs(self.receipt['ci_acceptance'], False)
        self.assertEqual('pending_not_accepted', self.receipt['ci_status'])

    def test_default_CI_gate_still_rejects_absent_failed_pending_or_wrong_source(self):
        plan = dict(self.plan, security_groups=[{'synthetic_offline_only': True}])
        for ci in ({}, {'source_head': self.head, 'applicable_ci_green': False},
                   {'source_head': self.head, 'status': 'pending'},
                   {'source_head': '0'*40, 'applicable_ci_green': True}):
            with self.subTest(ci=ci), self.assertRaisesRegex(checkpoint.lifecycle.Refused,
                    'exact_source_green_applicable_CI'):
                checkpoint.checkpoint_validation_mode(plan, ci, True)
        self.assertEqual('ci', checkpoint.checkpoint_validation_mode(plan,
            {'source_head': self.head, 'applicable_ci_green': True}, True))
        plan['validation_mode'] = 'ignore_ci'
        with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'checkpoint_validation_mode'):
            checkpoint.checkpoint_validation_mode(plan, {}, True)
        plan['validation_mode'] = 'human_directed_local'
        self.assertEqual('human_directed_local', checkpoint.checkpoint_validation_mode(plan, {}, True))
        plan['security_groups'] = []
        with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'approved_existing_security_group_snapshot'):
            checkpoint.checkpoint_validation_mode(plan, {}, True)

    def test_missing_failed_pending_mismatched_unreviewed_assessment_is_refused(self):
        self.plan.pop('local_validation')
        with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'local_validation_receipt_required'):
            self.validate()
        for field, value in [('build_exit', 1), ('build_exit', None), ('build_exit', False),
                             ('ctest_exit', 8), ('status', 'pending'), ('reviewed', False),
                             ('source_head', '0'*40), ('profile', 'unixodbc-redshift'),
                             ('ci_acceptance', True), ('ci_status', 'green'),
                             ('complete_applicable_offline', False)]:
            with self.subTest(field=field, value=value):
                old = dict(self.receipt); self.receipt[field] = value; self.refresh()
                with self.assertRaises(checkpoint.lifecycle.Refused): self.validate()
                self.receipt = old

    def test_changed_missing_or_empty_human_direction_and_evidence_are_refused(self):
        path = Path(self.direction['path']); path.write_text('changed after review')
        with self.assertRaises(checkpoint.lifecycle.Refused): self.validate()
        self.receipt['human_direction_ref'] = self.write('empty-direction', {'human_direction': ''})
        self.refresh()
        with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'retained_human_direction_required'):
            self.validate()
        self.receipt['human_direction_ref'] = self.write('restored-direction', {'human_direction': 'SYNTHETIC'})
        Path(self.evidence['path']).unlink(); self.refresh()
        with self.assertRaises((OSError, checkpoint.lifecycle.Refused)): self.validate()

    def test_assessment_must_bind_current_runtime_inventory_and_profile(self):
        for field in ('runtime_index_sha256', 'profiles_sha256'):
            old = self.receipt[field]; self.receipt[field] = '0'*64; self.refresh()
            with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'reviewed_local_runtime_inventory_and_profiles'):
                self.validate()
            self.receipt[field] = old

    def test_control_only_source_applicability_is_narrow(self):
        with patch.object(checkpoint.subprocess, 'check_output', return_value='tools/development/live_checkpoint.py\n'):
            checkpoint.same_validation_product('a'*40, 'b'*40)
        for path in ('sdk/security.cpp', 'tools/development/redshift_resource.py'):
            with patch.object(checkpoint.subprocess, 'check_output', return_value=path+'\n'):
                with self.assertRaises(checkpoint.lifecycle.Refused):
                    checkpoint.same_validation_product('a'*40, 'b'*40)

    def test_posix_stager_change_only_preserves_prior_windows_sequence(self):
        paths = ('tools/development/redshift_profile_inputs.py',
                 'tools/development/test_redshift_profile_inputs.py')
        with patch.object(checkpoint.subprocess, 'check_output', return_value='\n'.join(paths)):
            with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'local_validation_same_product_source'):
                checkpoint.same_validation_product('a'*40, 'b'*40)
            checkpoint.same_validation_product('a'*40, 'b'*40, windows_sequence=True)
        for path in ('tools/development/windows_live_backend.py',
                     'tools/development/redshift_windows.py',
                     'tools/development/redshift_fixture_factory.py',
                     'tools/development/redshift_resource.py', 'odbc/connection_string.cpp'):
            with self.subTest(path=path), patch.object(checkpoint.subprocess, 'check_output',
                    return_value='\n'.join((*paths, path))):
                with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'local_validation_same_product_source'):
                    checkpoint.same_validation_product('a'*40, 'b'*40, windows_sequence=True)

    def test_wsl_requires_coordinator_qualified_complete_windows_result(self):
        self.plan.pop('native_windows')
        self.plan['validation_profile'] = self.receipt['profile'] = 'unixodbc-redshift'; self.refresh()
        with self.assertRaisesRegex(checkpoint.lifecycle.Refused, 'windows_qualification_required_before_WSL'):
            self.validate()
        windows = dict(source_head=self.head, reviewed=True, status='completed', profile='windows-redshift',
            complete_applicable_windows_live=True, cleanup_complete=True, actual_exit=0, evidence=self.evidence)
        self.receipt['windows_success'] = self.write('windows', windows); self.refresh(); self.validate()
        for field, value in [('complete_applicable_windows_live', False), ('cleanup_complete', False),
                             ('actual_exit', 1), ('actual_exit', False), ('profile', 'unixodbc-redshift')]:
            previous = dict(windows); windows[field] = value
            self.receipt['windows_success'] = self.write('windows', windows); self.refresh()
            with self.assertRaises(checkpoint.lifecycle.Refused): self.validate()
            windows = previous

    def test_windows_public_path_mapping_refuses_relative_UNC_and_other_drives(self):
        for path in ('C:relative.json', r'\\host\share\receipt.json', r'D:\receipt.json'):
            with self.subTest(path=path), self.assertRaisesRegex(checkpoint.lifecycle.Refused,
                    'local_validation_absolute_C_drive'):
                checkpoint.validation_file(dict(path=path, sha256='0'*64))
        with patch.object(checkpoint, 'pinned', return_value=Path('synthetic')) as pin:
            checkpoint.validation_file(dict(path=r'C:\public\receipt.json', sha256='0'*64))
            self.assertEqual(pin.call_args.args[0]['path'],
                r'C:\public\receipt.json' if os.name == 'nt' else '/mnt/c/public/receipt.json')
        with patch.object(checkpoint, 'pinned', return_value=Path('synthetic')) as pin:
            checkpoint.validation_file(dict(path='/mnt/c/public/receipt.json', sha256='0'*64))
            self.assertEqual(pin.call_args.args[0]['path'],
                'C:/public/receipt.json' if os.name == 'nt' else '/mnt/c/public/receipt.json')


if __name__ == '__main__':
    unittest.main()
