"""Offline checks of profiles that load the owned driver through iODBC or unixODBC."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.development import redshift_fixture_factory as factory
from tools.development import redshift_profile_inputs as profile_inputs


def pin(path):
    return {'path': str(path), 'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest()}


class DriverManagerProfileTests(unittest.TestCase):
    def setUp(self):
        os.umask(0o077)
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name).resolve()
        self.root = root
        self.driver = root / 'libodbcpp.dylib'
        self.driver.write_bytes(b'synthetic driver')
        self.ca = root / 'ca.pem'
        self.ca.write_bytes(b'synthetic CA')
        review = root / 'review.json'
        review.write_text('{"synthetic_offline_only":true}')
        self.review = pin(review)
        self.config = {'admin_password': 'invented-admin', 'admin_user': 'odbcpp_pilot_admin',
                       'ca': str(self.ca), 'database': 'odbcpp_pilot', 'host': 'example.invalid',
                       'port': 5439, 'test_password': 'invented-test',
                       'test_user': 'odbcpp_pilot_test'}

    def tearDown(self):
        self.temp.cleanup()

    def profile(self, kind, manager, *, wchar_bytes=None):
        driver = None
        if manager is not None or kind == 'OWN_DSN':
            wchar_bytes = wchar_bytes or (4 if manager in (None, 'iODBC') else 2)
            driver = dict(pin(self.driver), wchar_bytes=wchar_bytes, kind='OWN')
        result = {'profile_name': 'REDSHIFT/synthetic', 'limits': dict(factory.CANONICAL_LIMITS),
                  'connection_environment': ['ODBCPP_REDSHIFT_TEST_CONNECTION'],
                  'uses_private_dsn': kind == 'OWN_DSN',
                  'variables': {'ODBCPP_TEST_DSN': 'DSN=RedshiftProd'} if kind == 'OWN_DSN' else {},
                  'review': self.review,
                  'private_input': {'kind': kind, 'driver': driver, 'password_rejection': False}}
        if manager is not None:
            result['driver_manager'] = manager
        return result

    def test_core_connection_names_the_pinned_driver_only_through_a_manager(self):
        direct = factory.build_profile_connection(self.config, self.profile('CORE_ENV', None))
        self.assertNotIn('DRIVER=', direct)
        for manager in ('iODBC', 'unixODBC'):
            value = factory.build_profile_connection(self.config, self.profile('CORE_ENV', manager))
            # Bare: iODBC 3.52.16 would dlopen a braced value with its braces.
            self.assertEqual(value, 'DRIVER=' + str(self.driver) + ';' + direct)
            # The invalid-password variant reaches the same driver.
            profile = self.profile('CORE_ENV', manager)
            profile['private_input']['password_rejection'] = True
            invalid = factory.build_profile_connection(self.config, profile, password_rejection=True)
            self.assertTrue(invalid.startswith('DRIVER=' + str(self.driver) + ';'))
            self.assertNotIn('PWD={invented-test}', invalid)

    def test_windows_manager_selects_the_registered_description_only(self):
        direct = factory.build_profile_connection(self.config, self.profile('CORE_ENV', None))
        profile = self.profile('CORE_ENV', 'Windows')
        profile['private_input']['password_rejection'] = True
        value = factory.build_profile_connection(self.config, profile,
                                                 registered_driver='ODBCPP Redshift')
        self.assertEqual(value, 'DRIVER={ODBCPP Redshift};' + direct)
        self.assertNotIn(str(self.driver), value)
        invalid = factory.build_profile_connection(self.config, profile, password_rejection=True,
                                                   registered_driver='ODBCPP Redshift')
        self.assertTrue(invalid.startswith('DRIVER={ODBCPP Redshift};'))
        self.assertNotIn('PWD={invented-test}', invalid)
        # Without the verified description the POSIX path form is refused.
        with self.assertRaisesRegex(factory.FactoryRefused, 'registered_driver_selector'):
            factory.build_profile_connection(self.config, profile)
        for selector in ('', 'a;b', 'x}', 'a=b', '{x}', 'x' * 129):
            with self.assertRaisesRegex(factory.FactoryRefused, 'registered_driver_description'):
                factory.build_profile_connection(self.config, profile, registered_driver=selector)
        # Direct and POSIX manager profiles never take a description.
        for manager in (None, 'iODBC', 'unixODBC'):
            with self.assertRaisesRegex(factory.FactoryRefused, 'registered_driver_selector'):
                factory.build_profile_connection(self.config, self.profile('CORE_ENV', manager),
                                                 registered_driver='ODBCPP Redshift')

    def test_posix_stager_refuses_a_windows_manager_profile(self):
        manifest = {'pins': {str(self.driver): pin(self.driver)['sha256']}}
        with self.assertRaises(profile_inputs.ProfileInputBlocked) as failure:
            profile_inputs.prepare_profile_inputs(
                self.root / 'profiles', self.config, factory,
                {'windows': self.profile('CORE_ENV', 'Windows')}, manifest)
        refusal = failure.exception.__context__
        self.assertIsInstance(refusal, factory.FactoryRefused)
        self.assertEqual(str(refusal), 'registered_driver_selector')
        # Refused before any private connection file was written.
        self.assertEqual(failure.exception.residue['profiles']['windows']['files'], [])
        profile_inputs.close_profile_inputs(failure.exception.residue)
        self.assertFalse((self.root / 'profiles').exists())

    def test_manager_profiles_refuse_mismatched_width_kind_and_unpinned_driver(self):
        cases = [self.profile('CORE_ENV', 'iODBC', wchar_bytes=2),
                 self.profile('CORE_ENV', 'unixODBC', wchar_bytes=4),
                 self.profile('CORE_ENV', 'Windows', wchar_bytes=4),
                 self.profile('OWN_DSN', 'Windows', wchar_bytes=4),
                 self.profile('CORE_ENV', 'Other')]
        package = self.profile('OWN_DSN', 'iODBC')
        package.update(uses_private_dsn=False, connection_environment=[], variables={})
        package['private_input']['kind'] = 'OWN_DM_FILE'
        unpinned = self.profile('CORE_ENV', 'iODBC')
        unpinned['private_input']['driver']['sha256'] = 'not-a-digest'
        quoted = self.profile('CORE_ENV', 'iODBC')
        quoted['private_input']['driver']['path'] = str(self.root / 'a;b' / 'libodbcpp.dylib')
        for profile in cases + [package, unpinned, quoted]:
            with self.assertRaises(factory.FactoryRefused):
                factory.validate_profile(profile)
        # A plain CORE_ENV profile still has no driver of its own.
        core = self.profile('CORE_ENV', None)
        core['private_input']['driver'] = dict(pin(self.driver), wchar_bytes=4, kind='OWN')
        with self.assertRaisesRegex(factory.FactoryRefused, 'CORE_has_no_DM_driver'):
            factory.validate_profile(core)

    def test_private_dsn_environment_and_registration_follow_the_manager(self):
        manifest = {'pins': {str(self.driver): pin(self.driver)['sha256']}}
        profiles = {'direct': self.profile('OWN_DSN', None),
                    'iodbc': self.profile('OWN_DSN', 'iODBC'),
                    'unixodbc': self.profile('OWN_DSN', 'unixODBC')}
        state = profile_inputs.prepare_profile_inputs(self.root / 'profiles', self.config,
                                                      factory, profiles, manifest)
        try:
            for key, relative in (('direct', False), ('iodbc', False), ('unixodbc', True)):
                part = state['profiles'][key]
                dsn = Path(part['directory']) / 'dsn'
                environment = profile_inputs.profile_private_environment(state, key)
                self.assertEqual(environment['ODBCINI'], str(dsn / 'odbc.ini'))
                self.assertEqual(environment['ODBCSYSINI'], str(dsn))
                self.assertEqual(environment['ODBCINSTINI'],
                                 'odbcinst.ini' if relative else str(dsn / 'odbcinst.ini'))
                registration = (dsn / 'odbcinst.ini').read_text()
                self.assertIn('Driver=' + str(self.driver) + '\n', registration)
                self.assertEqual('DriverUnicodeType=UCS4' in registration, key == 'iodbc')
        finally:
            profile_inputs.close_profile_inputs(state)
        self.assertFalse((self.root / 'profiles').exists())


if __name__ == '__main__':
    unittest.main()
