"""Offline sanitizer scheduling/evidence safety; never product assertions."""
import ast
from copy import deepcopy
import importlib.util
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
BASE = 'c82e4fe790c66cb9439d2f56c3b366163ffa3af7'
HELPER = Path(__file__).with_name('sanitizer_evidence.py')
h = None
if HELPER.exists():
    spec = importlib.util.spec_from_file_location('sanitizer_evidence', HELPER)
    h = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(h)


def jobs(text):
    section = text.split('\njobs:\n', 1)[1]
    return dict(re.findall(r'^  ([a-z][a-z-]*):\n(.*?)(?=^  [a-z][a-z-]*:|\Z)', section, re.M | re.S))


def enabled(block, event, flags):
    found = re.search(r'^    if: (.*)$', block, re.M)
    if not found:
        return True
    expression = found[1].replace('${{', '').replace('}}', '').strip()
    expression = expression.replace('github.event_name', repr(event))
    expression = re.sub(r'inputs\.([a-z_]+)', lambda m: repr(flags.get(m[1], False)), expression)
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    expression = re.sub(r'!(?!=)', ' not ', expression).strip()
    tree = ast.parse(expression, mode='eval')
    allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not,
               ast.Compare, ast.Eq, ast.NotEq, ast.Constant, ast.Load)
    assert all(isinstance(n, allowed) for n in ast.walk(tree)), expression
    return bool(eval(compile(tree, '<workflow boolean>', 'eval'), {'__builtins__': {}}, {}))


class SchedulingSafety(unittest.TestCase):
    def setUp(self):
        self.ci = (ROOT / '.github/workflows/ci.yml').read_text()
        self.old = subprocess.check_output(['git', 'show', BASE + ':.github/workflows/ci.yml'], cwd=ROOT, text=True)

    def test_default_off_and_every_existing_mode_unchanged(self):
        self.assertRegex(self.ci, r'run_sanitizer_only:\n(?:[^\n]*\n){1,5}        default: false')
        old, new = jobs(self.old), jobs(self.ci)
        for event in ['pull_request', 'workflow_dispatch']:
            for values in itertools.product([False, True], repeat=4):
                flags = dict(zip(['run_development_pilot', 'run_product_qualification',
                                  'run_windows_product_qualification', 'run_linked_server'], values))
                flags['run_sanitizer_only'] = False
                with self.subTest(event=event, flags=flags):
                    self.assertEqual({n for n,b in old.items() if enabled(b,event,flags)},
                                     {n for n,b in new.items() if enabled(b,event,flags)})

    def test_only_selector_runs_one_guarded_job_even_with_conflicts(self):
        for values in itertools.product([False, True], repeat=4):
            flags = dict(zip(['run_development_pilot', 'run_product_qualification',
                              'run_windows_product_qualification', 'run_linked_server'], values))
            flags['run_sanitizer_only'] = True
            self.assertEqual([n for n,b in jobs(self.ci).items() if enabled(b,'workflow_dispatch',flags)], ['sanitizer-only'])

    def test_all_legacy_full_bodies_and_sanitizer_profile_are_preserved(self):
        old, new = jobs(self.old), jobs(self.ci)
        for name in old:
            if name in ['ubuntu-development-pilot', 'ubuntu-product-qualification', 'windows-product-qualification']:
                continue
            strip = lambda b: re.sub(r'^    if: .*\n', '', b, flags=re.M)
            self.assertEqual(strip(old[name]), strip(new[name]), name)
        only = new['sanitizer-only']
        for text in ['timeout-minutes: 30', 'runs-on: ubuntu-latest', 'retention-days: 3',
                     'persist-credentials: false', 'if: always()', 'sanitizer_evidence.py']:
            self.assertIn(text, only)
        for text in ['services:', 'secrets:', 'actions/cache', 'self-hosted', 'postgres.ps1']:
            self.assertNotIn(text, only)
        self.assertIn("inputs.run_sanitizer_only", self.ci.split('jobs:',1)[0])
        self.assertIn('cancel-in-progress: false', self.ci)


class EvidenceSafety(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(h, 'evidence helper not yet implemented')
        self.env = {'SANITIZER_ONLY': 'true', 'SANITIZER_CONFLICT': 'false', 'SANITIZER_CACHE': '{}',
                    'SANITIZER_WORKFLOW_HEAD': 'a'*40, 'SANITIZER_PRODUCT_HEAD': BASE,
                    'SANITIZER_PRODUCT_BASE': 'b'*40, 'SANITIZER_SUBMITTED_AT': '2026-01-01T00:00:00Z',
                    'GITHUB_SHA': 'a'*40, 'GITHUB_REPOSITORY': 'vahid110/odbcpp',
                    'GITHUB_REPOSITORY_VISIBILITY': 'public', 'RUNNER_ENVIRONMENT': 'github-hosted',
                    'RUNNER_OS': 'Linux', 'RUNNER_ARCH': 'X64',
                    'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                    'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
                    'GITHUB_WORKSPACE': str(ROOT.parent),
                    'SANITIZER_WORKFLOW_ROOT': str(ROOT.parent/'sanitizer-workflow'),
                    'SANITIZER_PRODUCT_ROOT': str(ROOT.parent/'sanitizer-product')}

    def test_immutable_public_cold_request_and_conflicts_refused(self):
        h.request_guard(self.env)
        for key,value in [('SANITIZER_WORKFLOW_HEAD','main'),('SANITIZER_PRODUCT_HEAD','c'*40),
                          ('SANITIZER_PRODUCT_BASE','HEAD~1'),('GITHUB_SHA','c'*40),
                          ('SANITIZER_CONFLICT','true'),('SANITIZER_ONLY','false'),
                          ('SANITIZER_CACHE','{"restore":"x"}'),('GITHUB_REPOSITORY_VISIBILITY','private'),
                          ('RUNNER_ENVIRONMENT','self-hosted'),('RUNNER_OS','Windows'),('RUNNER_ARCH','ARM64'),
                          ('ASAN_OPTIONS','detect_leaks=0'),('UBSAN_OPTIONS','halt_on_error=0'),
                          ('SANITIZER_SUBMITTED_AT','2999-01-01T00:00:00Z'),
                          ('SANITIZER_WORKFLOW_ROOT','/tmp/untrusted-helper'),
                          ('SANITIZER_PRODUCT_ROOT','/tmp/other-product')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                h.request_guard(dict(self.env, **{key:value}))

    def test_source_guard_rejects_either_changed_source(self):
        before={'workflow':{'head':'a'},'product':{'head':'b'}}
        with tempfile.TemporaryDirectory() as tmp, patch.object(h,'EVIDENCE',Path(tmp)):
            h.write('identity.json',{'sources':before})
            for role in before:
                after=deepcopy(before);after[role]['head']='changed'
                with patch.object(h,'sources',return_value=after),self.assertRaises(ValueError):h.source_guard()

    def test_missing_new_unit_producer_and_disabled_inventory_refused(self):
        self.assertEqual(h.require_unit_sources({'test_new_sdk','test_new_adapter'}, ['test_new_sdk','test_new_adapter']), None)
        with self.assertRaises(ValueError):h.require_unit_sources({'test_new_sdk'}, ['test_new_sdk','test_new_adapter'])
        for bad in ['', 'Suite.\n  DISABLED_one\n', 'Suite.\n  One\n  One\n']:
            with self.assertRaises(ValueError):h.gtest_names(bad)
        self.assertEqual(h.gtest_names('Suite.\n  One\n'), ['Suite.One'])

    def test_xml_exact_cases_failure_skip_and_separate_binary_names(self):
        good='<testsuites tests="1"><testsuite><testcase classname="Suite" name="One"/></testsuite></testsuites>'
        with tempfile.TemporaryDirectory() as tmp, patch.object(h,'EVIDENCE',Path(tmp)):
            self.assertNotEqual(h.gtest_xml_path('test_sdk'),h.gtest_xml_path('test_adapter'))
            for bad in ['../test_x','test/x','test_x:bad']:
                with self.assertRaises(ValueError):h.gtest_xml_path(bad)
            p=Path(tmp)/'a.xml';p.write_text(good);self.assertEqual(h.xml_result(p,['Suite.One'])['count'],1)
            for bad in [good.replace('/>','><failure/></testcase>'),good.replace('/>','><skipped/></testcase>'),
                        good.replace('name="One"','name="Other"'),good.replace('tests="1"','tests="2"'),
                        '<!DOCTYPE x>'+good]:
                p.write_text(bad)
                with self.assertRaises(ValueError):h.xml_result(p,['Suite.One'])

    def test_failed_or_missing_phase_never_completes(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(h,'EVIDENCE',Path(tmp)):
            self.assertFalse(h.completion()[0])
            for phase in h.PHASES:h.write(phase+'.json',{'status':'passed'})
            self.assertFalse(h.completion()[0])
            h.write('results.json',{'actual':'validated'})
            self.assertTrue(h.completion()[0])
            h.write('build.json',{'status':'failed'});self.assertFalse(h.completion()[0])

    def test_failed_and_timeout_commands_keep_original_result_and_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(h,'EVIDENCE',Path(tmp)):
            for name,argv,limit in [('failure',[sys.executable,'-c','print("original failure");raise SystemExit(7)'],5),
                                    ('timeout',[sys.executable,'-c','import time;time.sleep(20)'],0.05)]:
                with self.assertRaises(RuntimeError):h.measure(name,argv,limit)
                rec=json.loads((Path(tmp)/(name+'-command.json')).read_text())
                self.assertTrue(rec['direct_terminal']);self.assertTrue(rec['cleanup']['group_absent'])
                self.assertEqual(rec['timed_out'],name=='timeout')
                if name=='failure':self.assertEqual(rec['exit'],7)

    def test_exported_cxx_instrumentation_cannot_be_reduced(self):
        good = {'file': 'unit.cpp', 'command': 'c++ -fsanitize=address,undefined -fno-omit-frame-pointer -g -c unit.cpp -o unit.o'}
        self.assertIsNone(h.validate_compile_commands([good]))
        for old,new in [('-fsanitize=address,undefined','-fsanitize=address'),
                        ('-fno-omit-frame-pointer',''),('-g','-g -fno-sanitize=undefined')]:
            with self.subTest(old=old), self.assertRaises(ValueError):
                h.validate_compile_commands([dict(good,command=good['command'].replace(old,new))])

    def test_sdk_objects_and_staged_headers_require_complete_proof(self):
        headers = {'header'+str(i): 'a'*64 for i in range(23)}
        objects = {'obj'+str(i): {'bytes':1,'sha256':'b'*64} for i in range(23)}
        self.assertEqual(h.validate_sdk(headers,headers,objects)['headers'],headers)
        for staged,actual_objects in [({},objects),(dict(headers,header0='c'*64),objects),(headers,{})]:
            with self.assertRaises(ValueError):h.validate_sdk(headers,staged,actual_objects)

    def test_bounded_command_log_keeps_original_final_diagnostic(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(h,'EVIDENCE',Path(tmp)), patch.object(h,'LOG_BYTES',64):
            with self.assertRaises(RuntimeError):
                h.measure('large',[sys.executable,'-c','print("x"*4096);print("FINAL_DIAGNOSTIC");raise SystemExit(9)'],5)
            self.assertIn('FINAL_DIAGNOSTIC',(Path(tmp)/'large.log').read_text())
            record=json.loads((Path(tmp)/'large-command.json').read_text())
            self.assertTrue(record['log_truncated'])
            self.assertEqual(record['exit'],9)
            self.assertTrue(record['cleanup']['group_absent'])

    def test_early_request_refusal_still_has_packagable_original_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            workspace=Path(tmp)
            env=dict(os.environ,**self.env)
            env.update(GITHUB_WORKSPACE=tmp,SANITIZER_WORKFLOW_ROOT=str(workspace/'sanitizer-workflow'),
                       SANITIZER_PRODUCT_ROOT=str(workspace/'sanitizer-product'),SANITIZER_CONFLICT='true')
            (workspace/'sanitizer-workflow').mkdir()
            result=subprocess.run([sys.executable,'-B',str(HELPER),'request'],env=env,cwd=workspace/'sanitizer-workflow',
                                  stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
            self.assertNotEqual(result.returncode,0)
            evidence=workspace/'sanitizer-evidence'
            record=json.loads((evidence/'request.json').read_text())
            self.assertEqual(record['status'],'failed')
            self.assertIn('exclusive',record['error'])
            with patch.object(h,'EVIDENCE',evidence):h.package()
            self.assertIn('request.json',json.loads((evidence/'evidence-index.json').read_text()))
            only=jobs((ROOT/'.github/workflows/ci.yml').read_text())['sanitizer-only']
            self.assertIn('path: sanitizer-evidence/',only)
            for name in ['Retain completion source guards and original failures', 'Bound text evidence and closed index on failure too']:
                step=only.split('- name: '+name,1)[1].split('- name:',1)[0]
                self.assertIn('working-directory: sanitizer-workflow',step)

    def test_text_package_rejects_binary_symlink_and_bound(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(h,'EVIDENCE',Path(tmp)):
            p=Path(tmp)/'log.txt';p.write_text('original text')
            h.package();self.assertIn('log.txt',json.loads((Path(tmp)/'evidence-index.json').read_text()))
            (Path(tmp)/'binary.exe').write_bytes(b'bin')
            with self.assertRaises(ValueError):h.package()
            (Path(tmp)/'binary.exe').unlink();(Path(tmp)/'link.txt').symlink_to(p)
            with self.assertRaises(ValueError):h.package()
            (Path(tmp)/'link.txt').unlink()
            with patch.object(h,'MAX_BYTES',1),self.assertRaises(ValueError):h.package()


if __name__=='__main__':unittest.main()
