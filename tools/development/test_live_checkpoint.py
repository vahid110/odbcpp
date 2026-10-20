"""Cloud-free normal CLI preparation/preflight and authority-boundary checks."""
import hashlib
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


class CheckpointTests(unittest.TestCase):
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
            wrong_region=json.loads(plan_path.read_text())
            wrong_region['resource']['region']='eu-central-1'
            wrong_path=root/'wrong-region.json';wrong_path.write_text(json.dumps(wrong_region))
            with self.assertRaisesRegex(checkpoint.lifecycle.Refused,'approved_region_eu_north_1'):
                checkpoint.read_plan(wrong_path)
            with self.assertRaises(FileExistsError):
                checkpoint.prepare_plan(recipe_path,plan_path)
            with self.assertRaises(checkpoint.lifecycle.Refused):
                checkpoint.read_plan(plan_path,require_ci=True)
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


if __name__ == '__main__':
    unittest.main()
