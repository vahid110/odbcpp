"""Actual temporary Git object proofs; synthetic held files/registry only.

No installed file is changed, registry queried, app launched or SQL executed.
"""
import contextlib
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import windows_powerbi_contract as contract
import windows_powerbi_owner as owner
from test_windows_powerbi_owner import example_plan


class Repository:
    def __init__(self,path):
        self.path=Path(path)
        self.env={key:value for key,value in os.environ.items() if not key.upper().startswith('GIT_')}
        self.env.update(GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL=os.devnull)
        self.git('init','--initial-branch=main')
        self.write('core/product.cpp','unchanged product\n')
        self.write('CMakeLists.txt','unchanged graph\n')
        self.write('tools/development/helper.py','old operational control\n')
        self.write('tests/integration/it_bind_col_real.cpp','old fixture SQL\n')
        self.base=self.commit()

    def git(self,*args):
        return subprocess.check_output([str(contract.GIT),'-c','user.name=Offline Qualification',
            '-c','user.email=offline@example.invalid','-c','commit.gpgsign=false',
            '-c','core.hooksPath='+os.devnull,'-C',str(self.path),*args],
            env=self.env,stderr=subprocess.PIPE,timeout=10).decode().strip()

    def write(self,path,value):
        p=self.path/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(value)

    def commit(self):
        self.git('add','--all');self.git('commit','-m','offline synthetic fixture')
        return self.git('rev-parse','HEAD')

    def ctx(self,head=None):
        return SimpleNamespace(repo=self.path,plan={'source_head':head or self.git('rev-parse','HEAD')})


class Applicability(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='odbcpp-app016-git-')
        self.addCleanup(self.temporary.cleanup)
        self.repo=Repository(self.temporary.name)

    def test_same_actual_source_and_clean_operational_descendant_are_proven(self):
        same=owner.installed_source_applicability(self.repo.ctx(),self.repo.base)
        self.assertEqual(same['changed_paths'],[])
        self.repo.write('tools/development/helper.py','new operational control\n')
        self.repo.write('tools/development/README.md','operational documentation\n')
        self.repo.write('tests/integration/it_bind_col_real.cpp','new session identity SQL\n')
        self.repo.write('tests/integration/it_metadata_real.cpp','new session identity SQL\n')
        current=self.repo.commit()
        proof=owner.installed_source_applicability(self.repo.ctx(),self.repo.base)
        self.assertEqual(proof['installed_source_head'],self.repo.base)
        self.assertEqual(proof['application_source_head'],current)
        self.assertTrue(proof['unchanged_product_build_inputs'])
        self.assertFalse(proof['installed_source_relabelled'])
        self.assertEqual(set(proof['changed_paths']),{'tools/development/helper.py',
            'tools/development/README.md','tests/integration/it_bind_col_real.cpp',
            'tests/integration/it_metadata_real.cpp'})

    def test_each_product_build_family_and_other_fixture_change_is_refused(self):
        paths=('sdk/contract.hpp','core/product.cpp','odbc/adapter.cpp','include/public.h',
               'CMakeLists.txt','cmake/Tests.cmake','packaging/driver.wxs',
               '.github/workflows/build.yml','tools/ci/package.py',
               'tools/development/CMakeLists.txt','tests/integration/it_other.cpp')
        for number,path in enumerate(paths):
            with self.subTest(path=path), tempfile.TemporaryDirectory(prefix='odbcpp-app016-family-') as directory:
                repo=Repository(directory);repo.write(path,'changed build input\n');repo.commit()
                with self.assertRaisesRegex(RuntimeError,'installed_product_build_inputs_changed'):
                    owner.installed_source_applicability(repo.ctx(),repo.base)

    def test_dirty_staged_untracked_and_wrong_actual_head_are_refused(self):
        self.repo.write('tools/development/helper.py','dirty control\n')
        with self.assertRaisesRegex(RuntimeError,'clean_exact_product_comparison_checkout'):
            owner.installed_source_applicability(self.repo.ctx(),self.repo.base)
        self.repo.git('add','--all')
        with self.assertRaisesRegex(RuntimeError,'clean_exact_product_comparison_checkout'):
            owner.installed_source_applicability(self.repo.ctx(),self.repo.base)
        current=self.repo.commit()
        with self.assertRaisesRegex(RuntimeError,'clean_exact_product_comparison_checkout'):
            owner.installed_source_applicability(self.repo.ctx(self.repo.base),self.repo.base)
        self.repo.write('untracked.txt','not admitted\n')
        with self.assertRaisesRegex(RuntimeError,'clean_exact_product_comparison_checkout'):
            owner.installed_source_applicability(self.repo.ctx(current),self.repo.base)

    def test_missing_blob_and_unrelated_commit_objects_cannot_be_report_proofs(self):
        with self.assertRaisesRegex(RuntimeError,'source_comparison_git'):
            owner.installed_source_applicability(self.repo.ctx(),'f'*40)
        blob=self.repo.git('rev-parse',self.repo.base+':core/product.cpp')
        with self.assertRaisesRegex(RuntimeError,'own_source_commit_object'):
            owner.installed_source_applicability(self.repo.ctx(),blob)
        self.repo.git('checkout','--orphan','unrelated')
        self.repo.write('tools/development/helper.py','unrelated control\n');self.repo.commit()
        with self.assertRaisesRegex(RuntimeError,'source_comparison_git'):
            owner.installed_source_applicability(self.repo.ctx(),self.repo.base)

    def test_inherited_git_repository_and_index_overrides_are_ignored(self):
        with patch.dict(owner.os.environ,{'GIT_DIR':str(self.repo.path/'missing.git'),
                                       'GIT_INDEX_FILE':str(self.repo.path/'missing.index')}):
            proof=owner.installed_source_applicability(self.repo.ctx(self.repo.base),self.repo.base)
        self.assertTrue(proof['unchanged_product_build_inputs'])

    def test_original_aggregate_comparison_deadline_and_subprocess_timeout_refuse(self):
        ctx=self.repo.ctx()
        with patch.object(owner.time,'monotonic',side_effect=[0,31]), \
             patch.object(owner.subprocess,'run') as child:
            with self.assertRaisesRegex(RuntimeError,'source_comparison_deadline'):
                owner.installed_source_applicability(ctx,self.repo.base)
            child.assert_not_called()
        with patch.object(owner.subprocess,'run',side_effect=subprocess.TimeoutExpired('git',30)):
            with self.assertRaises(subprocess.TimeoutExpired):
                owner.installed_source_applicability(ctx,self.repo.base)

    def installed_ctx(self,kind='own',inventory_head=None,receipt_head=None):
        plan=example_plan(kind)
        plan['source_head']=self.repo.git('rev-parse','HEAD')
        body=b'synthetic installed public driver'
        sha=lambda data:hashlib.sha256(data).hexdigest()
        driver=plan['driver'];driver['sha256']=sha(body)
        driver['payload']=[dict(path=driver['path'],sha256=sha(body))]
        blobs={driver['path']:body,plan['application']['path']:b'app',plan['ca']['path']:b'ca'}
        plan['application']['sha256']=sha(b'app');plan['ca']['sha256']=sha(b'ca')
        installed=inventory_head or self.repo.base
        inventory=dict(sourceRevision=installed,targetDatabase='REDSHIFT',
                       files=[dict(file='odbcpp.dll',sha256=sha(body))])
        if kind=='own':
            raw=json.dumps(inventory).encode();blobs[driver['inventory']['path']]=raw
            driver['inventory']['sha256']=sha(raw)
        receipt=dict(schema_version=1,status='VERIFIED_OWNED_INSTALL',independent_review='ACCEPT',
            kind=kind,source_head=(receipt_head or self.repo.base) if kind=='own' else None,
            driver=dict(path=driver['path'],sha256=driver['sha256']),payload=driver['payload'],
            installer_sha256=None if kind=='own' else contract.AWS_MSI_SHA,
            evidence=[dict(path='C:/synthetic/proof.json',sha256='a'*64)])
        raw=json.dumps(receipt).encode();blobs[driver['install_receipt']['path']]=raw
        driver['install_receipt']['sha256']=sha(raw)
        class Held:
            def __init__(self,path):
                self.path=str(path)
            def read(self):
                return blobs[self.path]
        ctx=SimpleNamespace(plan=plan,repo=self.repo.path,native=SimpleNamespace(HeldFile=Held),
            hold=lambda held:held,read=lambda path:blobs[str(path)],record=Mock())
        registry=SimpleNamespace(HKEY_LOCAL_MACHINE=1,KEY_READ=2,KEY_WOW64_64KEY=4,REG_SZ=1,
            OpenKey=lambda *args:contextlib.nullcontext('synthetic-key'),
            QueryValueEx=lambda key,name:(driver['path'],1) if name=='Driver' else ('Installed',1))
        return ctx,registry,blobs,receipt,inventory

    def test_real_inventory_and_receipt_heads_agree_before_actual_git_reuse_proof(self):
        self.repo.write('tools/development/helper.py','new control\n');current=self.repo.commit()
        ctx,registry,blobs,receipt,inventory=self.installed_ctx()
        with patch.dict('sys.modules',{'winreg':registry}):
            owner.hold_runtime(ctx)
        name,proof=ctx.record.call_args.args
        self.assertEqual(name,'installed-source-applicability.json')
        self.assertEqual(proof['installed_source_head'],self.repo.base)
        self.assertEqual(proof['application_source_head'],current)
        ctx,registry,*_=self.installed_ctx(receipt_head=current)
        with self.assertRaisesRegex(RuntimeError,'own_inventory_source'):
            owner.hold_runtime(ctx)

    def test_target_database_payload_hash_and_vendor_msi_guards_remain_effective(self):
        for failure in ('database','payload','inventory','review','vendor_msi'):
            with self.subTest(failure=failure):
                kind='aws2' if failure=='vendor_msi' else 'own'
                ctx,registry,blobs,receipt,inventory=self.installed_ctx(kind)
                driver=ctx.plan['driver']
                if failure=='payload':
                    blobs[driver['path']]=b'changed installed payload'
                elif failure=='database':
                    inventory['targetDatabase']='POSTGRESQL'
                elif failure=='inventory':
                    inventory['files'].append(dict(file='missing.dll',sha256='b'*64))
                elif failure=='review':
                    receipt['independent_review']='PENDING'
                else:
                    receipt['installer_sha256']='b'*64
                if kind=='own':
                    raw=json.dumps(inventory).encode();blobs[driver['inventory']['path']]=raw
                    driver['inventory']['sha256']=owner.sha(raw)
                raw=json.dumps(receipt).encode();blobs[driver['install_receipt']['path']]=raw
                driver['install_receipt']['sha256']=owner.sha(raw)
                with patch.dict('sys.modules',{'winreg':registry}), self.assertRaises(RuntimeError):
                    owner.hold_runtime(ctx)

    def test_vendor_exact_msi_path_does_not_inherit_own_source_reuse(self):
        ctx,registry,*_=self.installed_ctx('aws2')
        with patch.dict('sys.modules',{'winreg':registry}), \
             patch.object(owner,'installed_source_applicability',side_effect=AssertionError('vendor source reuse')):
            owner.hold_runtime(ctx)
        ctx.record.assert_not_called()

    def test_registered_image_and_installed_mapping_are_still_required(self):
        for failure in ('driver','mapping'):
            with self.subTest(failure=failure):
                ctx,registry,*_=self.installed_ctx()
                def query(key,name):
                    if name=='Driver':
                        return ('C:/foreign.dll',1) if failure=='driver' else (ctx.plan['driver']['path'],1)
                    return ('Missing',1)
                registry.QueryValueEx=query
                with patch.dict('sys.modules',{'winreg':registry}), self.assertRaises(RuntimeError):
                    owner.hold_runtime(ctx)
                ctx.record.assert_not_called()


if __name__=='__main__':
    unittest.main()
