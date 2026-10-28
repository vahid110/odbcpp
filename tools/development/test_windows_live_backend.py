"""Native offline custody/ACL/registry/job proof using invented inputs and owned probe paths."""
import csv
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest
import uuid
import winreg
import hashlib
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.development import windows_live_backend as backend
from tools.development import windows_native_job as job

# Load only the parser in this source tree; never fall back to cwd/sys.path.
try:
    _xml_spec = importlib.util.spec_from_file_location(
        'odbcpp_xml_evidence', Path(__file__).resolve().parents[1] / 'odbcpp_xml_evidence.py')
    _xml_module = importlib.util.module_from_spec(_xml_spec)
    _xml_spec.loader.exec_module(_xml_module)
    parse_xml_file = _xml_module.parse_xml_file
except Exception:
    raise RuntimeError('Bounded XML evidence parser unavailable') from None


class NativeBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        backend.SID = list(csv.reader(subprocess.check_output(
            ['whoami.exe','/user','/fo','csv','/nh'], text=True).splitlines()))[0][-1]
        backend.DESCRIPTION = 'ODBCPP Synthetic Workflow Probe'
        backend.current_identity()

    def test_relationship_observed_IDs_and_exact_owned_DSN_are_staged(self):
        from tools.development import redshift_fixture_factory as factory
        from tools.development.redshift_powerbi_partition import CASES
        from tools.development.redshift_ctest_lifecycle import Refused
        with tempfile.TemporaryDirectory(prefix='odbcpp-relationship-native-input-') as temporary:
            root = Path(temporary)
            profile = dict(profile_name='relationship-offline',
                limits=dict(factory.CANONICAL_LIMITS),
                connection_environment=['ODBCPP_TEST_DSN'],uses_private_dsn=True,
                variables=dict(ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP='1',
                    ODBCPP_REDSHIFT_POWERBI_PARENT_OID='1',
                    ODBCPP_REDSHIFT_POWERBI_FACT_OID='2'),
                review=dict(path=str(root/'review.json'),sha256='0'*64),
                private_input=dict(kind='OWN_DSN',password_rejection=False,
                    driver=dict(path=sys.executable,sha256='0'*64,wchar_bytes=2,kind='OWN')))
            observed = dict(profile['variables'], ODBCPP_REDSHIFT_POWERBI_PARENT_OID='6001',
                            ODBCPP_REDSHIFT_POWERBI_FACT_OID='6002')
            rows = [(dict(origin_id='relationship'+str(i),origin_image='owner',host_profile='one',
                          executable=sys.executable,sha256='0'*64,filter=case),
                     dict(case=case,env=None,marker=None,profile='control'),profile)
                    for i,case in enumerate(CASES)]
            payload = dict(schema_version=1,source_head='a'*40,run_id='windows-live-offline',
                config=dict(host='invented.invalid',port=5439,database='odbcpp_pilot',
                            test_user='odbcpp_pilot_test',test_password='invented-only',
                            ca=str(root/'invented-ca.pem')),
                case_variables=[dict(id='relationship'+str(i),variables=observed)
                                for i in range(2)])
            with patch.object(backend,'HEAD','a'*40,create=True), \
                 patch.object(backend,'APPLICABLE_COUNT',2,create=True), \
                 patch.object(backend,'DRIVER',Path(sys.executable),create=True), \
                 patch.object(backend,'DRIVER_SHA',
                     hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest(),create=True):
                backend.validate_payload(payload,rows)
                observed['ODBCPP_REDSHIFT_POWERBI_FACT_OID']='6001'
                with self.assertRaisesRegex(Refused,'distinct_OIDs'):
                    backend.validate_payload(payload,rows)
                observed['ODBCPP_REDSHIFT_POWERBI_FACT_OID']='6002'
                for different in (False,True):
                    tree=backend.PrivateTree()
                    run=tree.mkdir(root/('different' if different else 'identical'))
                    try:
                        if different:
                            payload['case_variables'][1]['variables']=dict(observed,
                                ODBCPP_REDSHIFT_POWERBI_FACT_OID='6003')
                            with self.assertRaisesRegex(RuntimeError,'shared_native_profile'):
                                backend.build_inputs(payload,rows,factory,tree,run)
                        else:
                            index, expected, dsn = backend.build_inputs(payload,rows,factory,tree,run)
                            self.assertEqual(len(expected['cases']),2)
                            self.assertEqual(dsn['UID'],'odbcpp_pilot_test')
                            self.assertEqual(len(index['profiles']),1)
                        # The real native custody tree contains one held file,
                        # never a duplicate handle/retirement for the selector.
                        self.assertEqual(len(tree.files),1)
                        values=json.loads(tree.files[0].read())['environment']
                        self.assertIn('ODBCPP_TEST_DSN=DSN='+backend.RegistryDsn.NAME+';',values)
                        self.assertIn('ODBCPP_REDSHIFT_POWERBI_PARENT_OID=6001',values)
                        self.assertIn('ODBCPP_REDSHIFT_POWERBI_FACT_OID=6002',values)
                        self.assertIn('ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER='+sys.executable,values)
                    finally:
                        for held in reversed(tree.files):
                            held.retire()
                            self.assertFalse(held.path.exists())
                        tree.files.clear()
                        for path,handle in list(reversed(tree.directories)):
                            tree.rmdir(path)
                        tree.close_handles()
                    self.assertFalse(run.exists())

    def test_windows_manager_profiles_select_the_registered_driver(self):
        from tools.development import redshift_fixture_factory as factory
        driver = Path(sys.executable)
        digest = hashlib.sha256(driver.read_bytes()).hexdigest()
        # The WSL plan names the driver by its mounted path; both forms map.
        mounted = '/mnt/' + driver.drive[0].lower() + driver.as_posix()[2:]
        with tempfile.TemporaryDirectory(prefix='odbcpp-manager-native-input-') as temporary:
            root = Path(temporary)
            def profile(kind, manager='Windows', path=mounted, sha256=digest, width=2,
                        rejection=False):
                result = dict(profile_name='manager-offline/' + kind,
                    limits=dict(factory.CANONICAL_LIMITS),
                    connection_environment=['ODBCPP_REDSHIFT_TEST_CONNECTION'],
                    uses_private_dsn=kind == 'OWN_DSN',
                    variables={'ODBCPP_TEST_DSN': 'DSN=RedshiftProd'} if kind == 'OWN_DSN' else {},
                    review=dict(path=str(root/'review.json'),sha256='0'*64),
                    private_input=dict(kind=kind,password_rejection=rejection,
                        driver=dict(path=path,sha256=sha256,wchar_bytes=width,kind='OWN')))
                if manager is not None:
                    result['driver_manager'] = manager
                return result
            def inputs(profiles):
                rows = [(dict(origin_id='m'+str(i),origin_image='dm_owner',host_profile='h'+str(i),
                              executable=sys.executable,sha256='0'*64,filter='Suite.Case'+str(i)),
                         dict(case='Suite.Case'+str(i),env=None,marker=None,profile='p'+str(i)),value)
                        for i,value in enumerate(profiles)]
                payload = dict(config=dict(host='invented.invalid',port=5439,database='odbcpp_pilot',
                                           test_user='odbcpp_pilot_test',test_password='invented-only',
                                           ca=str(root/'invented-ca.pem')),
                               case_variables=[dict(id=row['origin_id'],variables=dict(spec_profile['variables']))
                                               for row,_,spec_profile in rows])
                return payload, rows
            def staged(tree):
                return {held.path.name: json.loads(held.read())['environment'] for held in tree.files}
            def retire(tree):
                for held in reversed(tree.files):
                    held.retire()
                tree.files.clear()
                for path,handle in list(reversed(tree.directories)):
                    tree.rmdir(path)
                tree.close_handles()
            with patch.object(backend,'HEAD','a'*40,create=True), \
                 patch.object(backend,'DRIVER',driver,create=True), \
                 patch.object(backend,'DRIVER_SHA',digest,create=True):
                # The receiver compares paths as Windows does, ignoring case.
                variant = mounted[:7] + mounted[7:].upper()
                accepted = ((profile('CORE_ENV', rejection=True), profile('OWN_DSN', rejection=True)),
                            (profile('CORE_ENV', path=variant), profile('OWN_DSN', path=str(driver))))
                for number, pair in enumerate(accepted):
                    payload, rows = inputs(list(pair))
                    tree = backend.PrivateTree()
                    run = tree.mkdir(root/('accepted'+str(number)))
                    try:
                        index, expected, dsn = backend.build_inputs(payload,rows,factory,tree,run)
                        self.assertEqual(len(expected['cases']),2)
                        environments = staged(tree)
                        selector = 'DRIVER={'+backend.DESCRIPTION+'};'
                        core_values = dict(v.split('=',1) for v in environments['h0.json'])
                        dsn_values = dict(v.split('=',1) for v in environments['h1.json'])
                        names = [(core_values, 'ODBCPP_REDSHIFT_TEST_CONNECTION')]
                        if number == 0:
                            # CORE_ENV and OWN_DSN invalid-password variants alike.
                            names += [(core_values, 'ODBCPP_REDSHIFT_AUTH_INVALID_CONNECTION'),
                                      (dsn_values, 'ODBCPP_REDSHIFT_AUTH_INVALID_CONNECTION')]
                        else:
                            self.assertNotIn('ODBCPP_REDSHIFT_AUTH_INVALID_CONNECTION', core_values)
                        for values, name in names:
                            self.assertTrue(values[name].startswith(selector), name)
                            self.assertNotIn(str(driver), values[name])
                            # Verified TLS, profile limits and driver logging are retained.
                            for field in ('SSL={1};','SSLCAFILE={'+str(root/'invented-ca.pem')+'};',
                                          'MAXRESULTS=1;','LogLevel=Debug;','LogSink=Stderr;'):
                                self.assertIn(field, values[name])
                        for values, name in names[1:]:
                            self.assertNotIn('PWD={invented-only}', values[name])
                        # OWN_DSN keeps SQLConnect through the exact owned HKCU DSN.
                        self.assertEqual(dsn_values['ODBCPP_TEST_DSN'], 'DSN=RedshiftProd')
                        self.assertNotIn('ODBCPP_REDSHIFT_TEST_CONNECTION', dsn_values)
                        self.assertEqual(Path(dsn['DRIVER']), driver)
                        self.assertEqual(dsn['UID'],'odbcpp_pilot_test')
                    finally:
                        retire(tree)
                    self.assertFalse(run.exists())
                other_drive = '/mnt/' + ('d' if mounted[5] != 'd' else 'e') + mounted[6:]
                refused = (('posix_driver_manager_profile', profile('CORE_ENV', manager='unixODBC')),
                           ('posix_driver_manager_profile', profile('CORE_ENV', manager='iODBC', width=4)),
                           ('manager_profile_owned_driver', profile('CORE_ENV', sha256='1'*64)),
                           ('manager_profile_owned_driver', profile('CORE_ENV', path='/mnt/c/other/odbcpp.dll')),
                           ('manager_profile_owned_driver', profile('CORE_ENV', path=other_drive)),
                           ('manager_profile_owned_driver', profile('CORE_ENV', width=4)),
                           ('absolute_profile_path', profile('CORE_ENV', path='/mnt/C' + mounted[6:])),
                           ('absolute_profile_path', profile('CORE_ENV', path=mounted[:7] + '../' + mounted[7:])),
                           ('profile_path', profile('CORE_ENV', path=None)))
                for reason, value in refused:
                    # First and after an already staged valid case: no rebinding,
                    # and every staged input still retires.
                    for position in (0, 1):
                        cases = [profile('OWN_DSN')]
                        cases.insert(position, value)
                        payload, rows = inputs(cases)
                        tree = backend.PrivateTree()
                        run = tree.mkdir(root/(reason+str(position)))
                        try:
                            with self.assertRaisesRegex(RuntimeError, reason):
                                backend.build_inputs(payload,rows,factory,tree,run)
                            self.assertEqual(len(tree.files), position)
                            self.assertEqual(tree.pending, [])
                            staged_paths = [held.path for held in tree.files]
                        finally:
                            retire(tree)
                        self.assertFalse(any(path.exists() for path in staged_paths))
                        self.assertFalse(run.exists())
                # Conflicting owned DSN configurations still need separate partitions.
                other = profile('OWN_DSN')
                other['limits']['MAXRESULTS'] = 3
                payload, rows = inputs([profile('OWN_DSN', manager=None), other])
                tree = backend.PrivateTree()
                run = tree.mkdir(root/'conflict')
                try:
                    with self.assertRaisesRegex(RuntimeError, 'single_native_dsn_profile'):
                        backend.build_inputs(payload,rows,factory,tree,run)
                finally:
                    retire(tree)

    def test_configuration_refuses_an_unsafe_registered_description(self):
        native = dict(working_directory='C:/invented/work', source_directory=str(ROOT),
                      source_head='a'*40, sid=backend.SID,
                      driver=dict(path='C:/invented/odbcpp.dll', sha256='0'*64),
                      model='C:/invented/model.json', case_count=1, runtime_directories=[],
                      tools=dict(cmake='C:/invented/cmake.exe', ctest='C:/invented/ctest.exe',
                                 ninja='C:/invented/ninja.exe'))
        saved = backend.DESCRIPTION
        try:
            backend.configure_context(dict(native, driver_description='ODBCPP Redshift Beta (x64)'))
            for description in ('ODBCPP {Redshift}', 'ODBCPP;Redshift', 'ODBCPP Redshift ', ''):
                with self.assertRaisesRegex(RuntimeError, 'registered_driver_description'):
                    backend.configure_context(dict(native, driver_description=description))
        finally:
            backend.DESCRIPTION = saved

    def test_private_file_exact_bytes_acl_and_retirement(self):
        parent = Path(tempfile.mkdtemp(prefix='odbcpp-native-workflow-'))
        root = parent/'owned';tree = backend.PrivateTree()
        try:
            tree.mkdir(root)
            path = root/'invented-input.txt'
            tree.write(path, b'invented-only-no-real-credential')
            held = tree.files[-1]
            self.assertEqual(held.read(), b'invented-only-no-real-credential')
            held.verify()
            with self.assertRaises(OSError):
                path.write_bytes(b'collision')
            held.retire();tree.files.clear()
            self.assertFalse(path.exists())
            tree.rmdir(root)
        finally:
            tree.close_handles()
        self.assertFalse(root.exists());parent.rmdir()

    def test_exact_invented_HKCU_namespace_and_collision_preservation(self):
        namespace = r'SOFTWARE\ODBCPP-Workflow-Probe-'+uuid.uuid4().hex
        class Probe(backend.RegistryDsn):
            ROOT_KEY = namespace
            PARENT = namespace+r'\ODBC.INI'
            SOURCES = PARENT+r'\ODBC Data Sources'
            NAME = 'InventedDsn'
        probe = Probe()
        self.assertFalse(probe.exists(namespace))
        values = {'Driver':'C:/invented/no-driver.dll','PWD':'invented-only','SSL':'1'}
        try:
            probe.prepare(values);probe.verify()
            with self.assertRaises(RuntimeError):
                Probe().prepare({'Driver':'C:/collision.dll'})
            probe.verify()
        finally:
            probe.cleanup()
        self.assertFalse(probe.exists(namespace))

    def test_native_job_nonzero_and_timeout_reap_descendant(self):
        root = Path(tempfile.mkdtemp(prefix='odbcpp-native-job-'))
        results = []
        for name, code, bound, expected in (
            ('nonzero','raise SystemExit(17)',10,17),
            ('timeout',"import subprocess,sys,time; subprocess.Popen([sys.executable,'-c','import time;time.sleep(600)']);time.sleep(600)",1,124)):
            receipt = {'case':name}
            with (root/(name+'.log')).open('w') as log:
                actual = job.run([sys.executable,'-c',code],log,receipt,timeout=bound)
            self.assertEqual(actual, expected)
            self.assertTrue(receipt['treeEmpty'])
            self.assertTrue(receipt['heldProcessHandlesStopped'])
            self.assertEqual(receipt['jobActiveProcesses'], 0)
            if name == 'timeout':
                self.assertTrue(receipt['timedOut'])
                self.assertGreaterEqual(receipt['jobTotalProcesses'], 2)
            results.append(receipt)
        (root/'receipts.json').write_text(json.dumps(results,indent=2)+'\n')
        print('Native offline job evidence retained: '+str(root))

    def test_complete_native_CTest_nonzero_still_closes_all_inputs_and_DSN(self):
        image_value = os.environ.get('ODBCPP_NATIVE_OFFLINE_IMAGE')
        if not image_value:
            self.skipTest('Supply an existing offline test_live_package_inputs image for native collection smoke')
        image = Path(image_value).resolve(strict=True)
        parent = Path(tempfile.mkdtemp(prefix='odbcpp-native-collection-'))
        work = parent/'private';owner = backend.PrivateTree();owner.mkdir(work)
        try:
            filters = ['LivePackageImagesTest.ClassifiesDatabaseDriversWithoutOtherLibraries',
                       'LivePackageImagesTest.DoesNotExist',
                       'LivePackageEnvironmentTest.OwnsValuesAcrossEnvironmentChangesAndPreservesAbsence']
            head = subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip()
            from tools.development import redshift_fixture_factory as factory
            profile = dict(profile_name='synthetic-offline',limits=dict(factory.CANONICAL_LIMITS),
                connection_environment=['ODBCPP_TEST_DSN'],uses_private_dsn=True,variables={},
                review={'path':str(work/'synthetic-review.json'),'sha256':'0'*64},
                private_input=dict(kind='OWN_DSN',driver={'path':sys.executable,
                    'sha256':hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest(),
                    'wchar_bytes':2,'kind':'OWN'},password_rejection=False))
            cases=[dict(id='offline'+str(i),image='synthetic',profile='host'+str(i),
                        gtest_filter=name,timeout_seconds=40) for i,name in enumerate(filters)]
            index=dict(schema_version=1,images={'synthetic':dict(executable=str(image),
                sha256=hashlib.sha256(image.read_bytes()).hexdigest())},profiles={},cases=cases)
            expected=dict(schema_version=1,cases=[{k:v for k,v in case.items() if k!='timeout_seconds'} for case in cases])
            specs=[dict(ctest_id=case['id'],case=case['gtest_filter'],profile='control',
                        case_seconds=40,env=None,marker=None) for case in cases]
            model=work/'model.json';model.write_text(json.dumps(dict(source_head=head,index=index,
                expected=expected,specs=specs,profiles={'control':profile})))
            native=dict(working_directory=str(work),source_directory=str(ROOT),source_head=head,
                sid=backend.SID,driver=dict(path=sys.executable,sha256='0'*64),
                driver_description=backend.DESCRIPTION,model=str(model),case_count=3,
                tools=dict(cmake=shutil.which('cmake'),ctest=shutil.which('ctest'),
                    ninja='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'),
                runtime_directories=[str(image.parent),str(Path(sys.executable).parent),
                                     'C:/Program Files/FireDaemon OpenSSL 3/bin'])
            backend.configure_context(native)
            namespace=r'SOFTWARE\ODBCPP-Workflow-Collection-Probe-'+uuid.uuid4().hex
            class Probe(backend.RegistryDsn):
                ROOT_KEY=namespace;PARENT=namespace+r'\ODBC.INI'
                SOURCES=PARENT+r'\ODBC Data Sources';NAME='InventedDsn'
            authority=dict(execution_timeout_seconds=90)
            payload=dict(schema_version=1,source_head=head,run_id='windows-live-offline',
                config=dict(host='synthetic.invalid',port=5439,database='odbcpp_pilot',
                    test_user='odbcpp_pilot_test',test_password='invented-only',ca=str(work/'invented-ca.pem')),
                case_variables=[dict(id=case['id'],variables={}) for case in cases])
            # Explicit fake-provider boundary: no installed driver, real DSN,
            # admission, credentials or resource is used. All native file/job/
            # CMake/CTest/finally behavior below is real and assertions unchanged.
            with patch.object(backend,'registration_matches'), \
                 patch.object(backend,'verify_admission',return_value=[]), \
                 patch.object(backend,'RegistryDsn',Probe):
                result=backend.execute(authority,payload)
            receipt=json.loads((work/'windows-live-offline/native-receipt.json').read_text())
            self.assertEqual(result,8)
            self.assertEqual(receipt['ctest_invocations'],1)
            self.assertEqual([j['phase'] for j in receipt['jobs']],['configure','ctest'])
            self.assertTrue(receipt['native_cleanup_complete'])
            self.assertTrue(receipt['private_inputs_closed'])
            self.assertTrue(receipt['dsn_closed'])
            self.assertTrue(receipt['lease_released'])
            self.assertFalse((work/'windows-live-active-lease.json').exists())
            self.assertFalse(Probe().exists(namespace))
            rows=list(parse_xml_file(work/'windows-live-offline/ctest-results.xml').iter('testcase'))
            self.assertEqual(len(rows),3)
            self.assertIsNone(rows[0].find('failure'))
            self.assertIsNotNone(rows[1].find('failure'))
            self.assertIsNone(rows[2].find('failure'))
            print('Native collection evidence retained: '+str(work))
        finally:
            owner.close_handles()


if __name__ == '__main__':
    unittest.main()
