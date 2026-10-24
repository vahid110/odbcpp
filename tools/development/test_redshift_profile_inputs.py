"""POSIX fixture custody and real-manager lookup; dummy files, no database I/O."""
import contextlib
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[2]))
from tools.development import redshift_profile_inputs as inputs
from tools.development import redshift_fixture_factory as factory
from tools.development.redshift_ctest_inputs import LoggingFactory
from tools.development.redshift_ctest_lifecycle import Refused


def manager_probe(library):
    """Isolated process: only the selected manager may load an invalid dummy .so."""
    manager = ctypes.CDLL(library)
    short, handle = ctypes.c_short, ctypes.c_void_p
    manager.SQLAllocHandle.argtypes = [short, handle, ctypes.POINTER(handle)]
    manager.SQLAllocHandle.restype = short
    manager.SQLSetEnvAttr.argtypes = [handle, ctypes.c_int, handle, ctypes.c_int]
    manager.SQLSetEnvAttr.restype = short
    manager.SQLDriverConnect.argtypes = [handle, handle, ctypes.c_char_p, short,
                                        ctypes.c_void_p, short, ctypes.POINTER(short), ctypes.c_ushort]
    manager.SQLDriverConnect.restype = short
    manager.SQLGetDiagRec.argtypes = [short, handle, short, ctypes.c_void_p,
                                     ctypes.POINTER(ctypes.c_int), ctypes.c_void_p, short, ctypes.POINTER(short)]
    manager.SQLGetDiagRec.restype = short
    manager.SQLFreeHandle.argtypes = [short, handle]
    manager.SQLFreeHandle.restype = short
    env, dbc = handle(), handle()
    if manager.SQLAllocHandle(1, None, ctypes.byref(env)) != 0:
        raise RuntimeError('manager_environment_allocation')
    try:
        if manager.SQLSetEnvAttr(env, 200, handle(3), 0) != 0:
            raise RuntimeError('manager_ODBC3_attribute')
        if manager.SQLAllocHandle(2, env, ctypes.byref(dbc)) != 0:
            raise RuntimeError('manager_connection_allocation')
        try:
            length = short()
            code = manager.SQLDriverConnect(dbc, None, b'DSN=RedshiftProd;', -3,
                                            None, 0, ctypes.byref(length), 0)
            diagnostics = []
            for number in range(1, 9):
                state, message = ctypes.create_string_buffer(6), ctypes.create_string_buffer(4096)
                native = ctypes.c_int()
                result = manager.SQLGetDiagRec(2, dbc, number, state, ctypes.byref(native),
                                               message, len(message), ctypes.byref(length))
                if result not in (0, 1):
                    break
                diagnostics.append(dict(state=state.value.decode('ascii'),
                                        message=message.value.decode('utf-8', errors='replace')))
            return dict(returncode=code, diagnostics=diagnostics)
        finally:
            manager.SQLFreeHandle(2, dbc)
    finally:
        manager.SQLFreeHandle(1, env)


@unittest.skipUnless(os.name == 'posix', 'POSIX private profile custody and managers')
class ProfileInputsTests(unittest.TestCase):
    @contextlib.contextmanager
    def owned(self, width):
        with tempfile.TemporaryDirectory(prefix='odbcpp-dsn-offline-') as temporary:
            root = Path(temporary).resolve()
            driver = root / 'invalid-fixture-driver.so'
            driver.write_bytes(b'Intentionally invalid synthetic library; cannot execute or connect.\n')
            digest = hashlib.sha256(driver.read_bytes()).hexdigest()
            config = dict(host='fixture.invalid', port=5439, database='odbcpp_pilot',
                          test_user='odbcpp_pilot_test', test_password='dummy-fixture-password',
                          admin_user='odbcpp_pilot_admin', admin_password='dummy-unused-admin',
                          ca=str(root / 'dummy-ca.pem'))
            profile = dict(profile_name='REDSHIFT/offline-private-DSN', limits=factory.limits_for(1),
                connection_environment=['ODBCPP_REDSHIFT_TEST_CONNECTION'], uses_private_dsn=True,
                variables={'ODBCPP_TEST_DSN': 'DSN=RedshiftProd;'},
                review=dict(path=str(root / 'synthetic-review.json'), sha256='a' * 64),
                private_input=dict(kind='OWN_DSN', password_rejection=False,
                    driver=dict(path=str(driver), sha256=digest, wchar_bytes=width, kind='OWN')))
            logged = LoggingFactory(factory)
            state = inputs.prepare_profile_inputs(root / 'private', config, logged,
                {'p': profile}, {'pins': {str(driver): digest}})
            try:
                yield root, state, profile, config, logged
            finally:
                if not state['closed']:
                    inputs.close_profile_inputs(state, readers_known=True)

    def test_manager_specific_selector_keeps_exact_DSN_bytes_TLS_caps_and_logging(self):
        for width in (2, 4):
            with self.subTest(width=width), self.owned(width) as (_, state, profile, config, logged):
                part = state['profiles']['p']
                d = Path(part['directory']) / 'dsn'
                expected = dict(ODBCINI=str(d / 'odbc.ini'), ODBCSYSINI=str(d),
                    ODBCINSTINI='odbcinst.ini' if width == 2 else str(d / 'odbcinst.ini'))
                self.assertEqual(expected, inputs.profile_private_environment(state, 'p'))
                documents = logged.dsn_documents(config, profile['limits'], profile['private_input']['driver']['path'])
                self.assertEqual([text.encode('utf-8') for text in documents],
                                 [row['body'] for row in part['dsn']['files']])
                self.assertIn('SSL=1\n', documents[0])
                self.assertIn('MAXROWS=1000000\n', documents[0])
                self.assertIn('LogQueries=false\n', documents[0])
                self.assertEqual(0o700, d.stat().st_mode & 0o777)
                for row in part['dsn']['files']:
                    self.assertEqual(0o600, Path(row['path']).stat().st_mode & 0o777)
                inputs.close_profile_inputs(state, readers_known=True)
                self.assertFalse(Path(state['directory']).exists())

    def test_changed_manager_selector_is_refused_before_return_or_cleanup(self):
        for width in (2, 4):
            for key, value in (('ODBCINSTINI', 'unrelated.ini'), ('ODBCSYSINI', '/unrelated'),
                               ('ODBCINI', '/unrelated/odbc.ini'), ('HOME', '/unrelated'),
                               ('SYSODBCINI', '/unrelated/odbc.ini')):
                with self.subTest(width=width, key=key), self.owned(width) as (_, state, _, _, _):
                    env = state['profiles']['p']['environment']
                    before = dict(env)
                    env[key] = value
                    try:
                        with self.assertRaisesRegex(Refused, 'own_dsn_private_selectors_changed'):
                            inputs.profile_private_environment(state, 'p')
                        with self.assertRaisesRegex(Refused, 'own_dsn_private_selectors_changed'):
                            inputs.close_profile_inputs(state, readers_known=True)
                        self.assertTrue(Path(state['directory']).exists())
                    finally:
                        env.clear()
                        env.update(before)

    def test_unknown_readers_and_replaced_DSN_preserve_inputs(self):
        with self.owned(2) as (_, state, _, _, _):
            with self.assertRaisesRegex(Refused, 'private_input_readers_unknown'):
                inputs.close_profile_inputs(state, readers_known=False)
            row = state['profiles']['p']['dsn']['files'][0]
            path = Path(row['path'])
            original = path.with_name('original-held-dsn')
            path.rename(original)
            path.write_bytes(row['body'])
            path.chmod(0o600)
            try:
                with self.assertRaisesRegex(Refused, 'private_dsn_changed'):
                    inputs.validate_profile_inputs(state)
                self.assertTrue(path.exists())
            finally:
                path.unlink()
                original.rename(path)

    def probe(self, library, state, *, previous_unix_selector=False):
        env = dict(PATH='/usr/bin:/bin', LANG='C', LC_ALL='C', HOME=str(Path(state['directory']).parent))
        env.update(inputs.profile_private_environment(state, 'p'))
        if previous_unix_selector:
            env['ODBCINSTINI'] = str(Path(state['profiles']['p']['directory']) / 'dsn' / 'odbcinst.ini')
        result = subprocess.run([sys.executable, '-B', str(Path(__file__).resolve()),
            '--manager-probe', str(library)], env=env, stdin=subprocess.DEVNULL,
            capture_output=True, text=True, timeout=10, check=False)
        self.assertEqual(0, result.returncode, result.stderr)
        return json.loads(result.stdout)

    def test_real_unixODBC_reaches_only_invalid_pinned_driver_and_original_selector_fails(self):
        library = Path('/usr/lib/x86_64-linux-gnu/libodbc.so.2')
        if not library.is_file():
            self.skipTest('unixODBC real manager unavailable at explicit system path')
        with self.owned(2) as (_, state, profile, _, _):
            previous = self.probe(library, state, previous_unix_selector=True)
            self.assertEqual(-1, previous['returncode'])
            self.assertTrue(any(row['state'] == 'IM002' for row in previous['diagnostics']), previous)
            corrected = self.probe(library, state)
            self.assertEqual(-1, corrected['returncode'])
            self.assertFalse(any(row['state'] == 'IM002' for row in corrected['diagnostics']), corrected)
            self.assertTrue(any(profile['private_input']['driver']['path'] in row['message']
                                for row in corrected['diagnostics']), corrected)

    def test_real_iODBC_preserves_absolute_selector_lookup(self):
        library = Path.home() / '.local/opt/iodbc-3.52.16/lib/libiodbc.so'
        if not library.is_file():
            self.skipTest('iODBC real manager unavailable at explicit installed prefix')
        with self.owned(4) as (_, state, profile, _, _):
            observed = self.probe(library, state)
            self.assertEqual(-1, observed['returncode'])
            self.assertTrue(any(row['state'] == 'IM003' for row in observed['diagnostics']), observed)
            self.assertFalse(any(row['state'] == 'IM002' for row in observed['diagnostics']), observed)
            self.assertTrue(any(profile['private_input']['driver']['path'] in row['message']
                                for row in observed['diagnostics']), observed)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--manager-probe':
        print(json.dumps(manager_probe(sys.argv[2])))
    else:
        unittest.main()
