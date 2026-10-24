"""POSIX offline controls only: all AWS executions use a local fake process."""
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

if os.name == 'posix':
    from tools.development import redshift_operator_session as session
    from tools.redshift.pilot_runtime import ProcessResult


@unittest.skipUnless(os.name == 'posix', 'POSIX custody contract')
class OperatorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.base.chmod(0o700)
        self.bootstrap_parent = self.base / 'bootstrap'
        self.bootstrap_parent.mkdir(mode=0o700)
        self.bootstrap = self.bootstrap_parent / 'credentials.ini'
        self.bootstrap.write_text('[odbcpp-redshift-pilot]\naws_access_key_id=AKIA' + 'B'*16 +
                                  '\naws_secret_access_key=bootstrap-secret\n')
        self.bootstrap.chmod(0o600)
        self.original = self.bootstrap.read_bytes()
        self.aws = self.base / 'aws'
        self.aws.write_bytes(b'fake-pinned-cli')
        self.aws.chmod(0o700)
        self.output = self.base / 'issued'
        self.identity = {'Account': session.ACCOUNT, 'Arn': session.ARN,
                         'UserId': 'AIDA' + 'X'*17}
        self.expiration = (datetime.now(timezone.utc) + timedelta(seconds=6600)).isoformat()
        self.credentials = {'AccessKeyId': 'ASIA' + 'T'*16,
                            'SecretAccessKey': 'temporary-secret', 'SessionToken': 'private-token',
                            'Expiration': self.expiration}
        self.calls = []
        self.transform = lambda index, response: response
        self.result = ProcessResult(0, False)
        for name, value in [('BOOTSTRAP', str(self.bootstrap)), ('AWS', str(self.aws)),
                            ('AWS_SHA256', hashlib.sha256(self.aws.read_bytes()).hexdigest())]:
            control = patch.object(session, name, value)
            control.start()
            self.addCleanup(control.stop)
        control = patch.object(session, 'bounded_process', side_effect=self.fake)
        control.start()
        self.addCleanup(control.stop)

    def fake(self, argv, *, env, seconds, output):
        index = len(self.calls)
        self.calls.append((argv, env, seconds))
        response = {'Credentials': self.credentials} if index == 1 else self.identity
        response = self.transform(index, response)
        fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(response if isinstance(response, str) else json.dumps(response))
        return self.result(index) if callable(self.result) else self.result

    def run_issue(self):
        result = session.issue(str(self.output))
        record = self.output / (result.get('failure_metadata_file') or 'metadata.json')
        self.assertEqual(json.loads(record.read_text()), result)
        self.assertEqual(self.bootstrap.read_bytes(), self.original)
        self.assertNotIn('bootstrap-secret', json.dumps(result))
        self.assertNotIn('temporary-secret', json.dumps(result))
        self.assertNotIn('private-token', json.dumps(result))
        return result

    def failed_clean(self, reason, calls):
        result = self.run_issue()
        self.assertFalse(result['verified'])
        self.assertEqual(result['status'], reason)
        self.assertEqual(len(self.calls), calls)
        self.assertTrue(result['readers_reaped'])
        self.assertTrue(result['temporary_outputs_retired'])
        self.assertEqual([p.name for p in self.output.iterdir()], ['metadata.json'])

    def test_success_exact_three_calls_protected_publish_and_clean_environment(self):
        with patch.dict(os.environ, {'AWS_ACCESS_KEY_ID': 'ambient', 'AWS_CA_BUNDLE': '/bad',
                                    'AWS_ENDPOINT_URL': 'http://bad', 'HTTPS_PROXY': 'http://bad'}):
            result = self.run_issue()
        self.assertTrue(result['verified'])
        self.assertEqual(result['expires_at'], self.expiration.replace('+00:00', 'Z'))
        self.assertEqual(set(p.name for p in self.output.iterdir()), {'credentials.ini', 'metadata.json'})
        self.assertEqual(result['aws_calls_started'], 3)
        self.assertEqual([call[0][2] for call in self.calls],
                         ['get-caller-identity', 'get-session-token', 'get-caller-identity'])
        self.assertIn('--duration-seconds', self.calls[1][0])
        self.assertIn('6600', self.calls[1][0])
        for argv, env, seconds in self.calls:
            self.assertEqual(argv[0], str(self.aws))
            self.assertIn(session.ENDPOINT, argv)
            self.assertIn(session.REGION, argv)
            self.assertNotIn('--no-verify-ssl', argv)
            self.assertEqual(argv[argv.index('--cli-connect-timeout')+1], '5')
            self.assertEqual(argv[argv.index('--cli-read-timeout')+1], '5')
            self.assertLessEqual(seconds, 20)
            self.assertEqual(env['AWS_MAX_ATTEMPTS'], '1')
            self.assertEqual(env['AWS_EC2_METADATA_DISABLED'], 'true')
            self.assertNotIn('AWS_ACCESS_KEY_ID', env)
            self.assertNotIn('AWS_CA_BUNDLE', env)
            self.assertNotIn('AWS_ENDPOINT_URL', env)
            self.assertNotIn('HTTPS_PROXY', env)
        for path in self.output.iterdir():
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(path.stat().st_nlink, 1)

    def test_root_identity_refused_before_issuance(self):
        self.identity['Arn'] = 'arn:aws:iam::385207570137:root'
        self.failed_clean('wrong_identity', 1)

    def test_wrong_account_and_assumed_role_refused(self):
        for field, value in [('Account', '111111111111'), ('Arn', session.ARN + '/other')]:
            with self.subTest(field=field):
                original = self.identity[field]
                self.identity[field] = value
                self.output = self.base / field
                self.calls.clear()
                self.failed_clean('wrong_identity', 1)
                self.identity[field] = original

    def test_temporary_identity_mismatch_retires_credentials(self):
        self.transform = lambda index, response: dict(response, UserId='AIDA'+'Y'*17) if index == 2 else response
        self.failed_clean('identity_changed', 3)

    def test_expired_response_never_used(self):
        self.credentials['Expiration'] = (datetime.now(timezone.utc)-timedelta(seconds=1)).isoformat()
        self.failed_clean('invalid_expiry', 2)

    def test_excessive_duration_and_naive_expiry_refused(self):
        for index, expiry in enumerate([(datetime.now(timezone.utc)+timedelta(hours=12)).isoformat(),
                                       '2026-10-24T20:00:00']):
            with self.subTest(expiry=expiry):
                self.output = self.base / ('expiry' + str(index))
                self.calls.clear()
                self.credentials['Expiration'] = expiry
                self.failed_clean('invalid_expiry', 2)

    def test_duplicate_json_keys_refused(self):
        self.transform = lambda index, response: '{"Account":"385207570137","Account":"x"}'
        self.failed_clean('invalid_response', 1)

    def test_nonzero_and_timeout_are_not_retried_after_reaping(self):
        for index, result in enumerate([ProcessResult(3, False), ProcessResult(-9, True)]):
            with self.subTest(result=result):
                self.output = self.base / ('failure' + str(index))
                self.calls.clear()
                self.result = result
                self.failed_clean('aws_call_failed', 1)
                facts = json.loads((self.output / 'metadata.json').read_text())['calls']
                self.assertEqual(facts, [{'operation': 'get-caller-identity',
                    'returncode': result.returncode, 'timed_out': result.timed_out,
                    'readers_reaped': True}])

    def test_issuance_and_verification_errors_retire_all_exact_outputs(self):
        for failed_index in (1, 2):
            with self.subTest(failed_index=failed_index):
                self.output = self.base / ('stage' + str(failed_index))
                self.calls.clear()
                self.result = lambda index: ProcessResult(9 if index == failed_index else 0, False)
                self.failed_clean('aws_call_failed', failed_index + 1)
                facts = json.loads((self.output / 'metadata.json').read_text())['calls']
                self.assertEqual([call['returncode'] for call in facts], [0]*failed_index + [9])
                self.assertTrue(all(call['readers_reaped'] for call in facts))

    def test_oversized_reaped_response_is_removed_without_exposing_raw_output(self):
        self.transform = lambda index, response: 'secret-raw' * 3000
        self.failed_clean('unsafe_file', 1)

    def test_unknown_process_cleanup_retains_private_inputs_and_never_publishes(self):
        with patch.object(session, 'bounded_process', side_effect=RuntimeError('secret-diagnostic')):
            result = self.run_issue()
        self.assertEqual(result['status'], 'process_cleanup_unknown')
        self.assertFalse(result['readers_reaped'])
        self.assertFalse(result['temporary_outputs_retired'])
        self.assertEqual(result['calls'], [{'operation': 'get-caller-identity',
                          'returncode': None, 'timed_out': None, 'readers_reaped': False}])
        self.assertNotIn('secret-diagnostic', json.dumps(result))
        self.assertFalse((self.output / 'credentials.ini').exists())
        self.assertTrue((self.output / 'bootstrap.private').is_file())

    def test_replayed_output_directory_is_not_adopted_or_changed(self):
        self.output.mkdir(mode=0o700)
        marker = self.output / 'metadata.json'
        marker.write_text('original')
        with self.assertRaises(FileExistsError):
            session.issue(str(self.output))
        self.assertEqual(marker.read_text(), 'original')
        self.assertEqual(self.calls, [])

    def test_unsafe_bootstrap_mode_rejected(self):
        self.bootstrap.chmod(0o640)
        self.failed_clean('unsafe_file', 0)

    def test_unsafe_bootstrap_parent_rejected(self):
        self.bootstrap_parent.chmod(0o755)
        self.failed_clean('unsafe_directory', 0)

    def test_bootstrap_hardlink_rejected(self):
        os.link(self.bootstrap, self.bootstrap_parent / 'extra')
        self.failed_clean('unsafe_file', 0)

    def test_bootstrap_symlink_rejected(self):
        self.bootstrap.rename(self.bootstrap_parent / 'original')
        self.bootstrap.symlink_to('original')
        self.failed_clean('issuer_failed', 0)

    def test_extra_profile_provider_or_session_token_rejected(self):
        baseline = self.original
        for index, extra in enumerate(['[other]\naws_access_key_id=x\n',
                                        'credential_process=anything\n', 'aws_session_token=old-token\n']):
            with self.subTest(extra=extra):
                self.output = self.base / ('profile' + str(index))
                self.bootstrap.write_bytes(baseline + extra.encode())
                self.original = self.bootstrap.read_bytes()
                self.calls.clear()
                result = self.run_issue()
                self.assertFalse(result['verified'])
                self.assertEqual(self.calls, [])
                self.assertFalse((self.output / 'credentials.ini').exists())

    def test_temporary_bootstrap_key_is_not_accepted_as_long_term(self):
        self.bootstrap.write_bytes(self.original.replace(b'AKIA', b'ASIA'))
        self.original = self.bootstrap.read_bytes()
        self.failed_clean('invalid_bootstrap', 0)

    def test_unsafe_output_parent_never_creates_directory(self):
        self.base.chmod(0o755)
        with self.assertRaisesRegex(session.Refused, 'unsafe_directory'):
            session.issue(str(self.output))
        self.assertFalse(self.output.exists())
        self.assertFalse(self.calls)

    def test_no_follow_directory_traversal_rejects_bootstrap_parent_alias(self):
        alias = self.base / 'alias'
        alias.symlink_to(self.bootstrap_parent, target_is_directory=True)
        with patch.object(session, 'BOOTSTRAP', str(alias / 'credentials.ini')):
            self.failed_clean('source_unavailable', 0)

    def test_wrong_cli_hash_blocks_before_any_aws_call(self):
        self.aws.write_bytes(b'changed')
        self.failed_clean('tool_changed', 0)

    def test_failed_tool_acquisition_closes_already_held_bootstrap(self):
        self.aws.unlink()
        before = set(os.listdir('/proc/self/fd'))
        self.failed_clean('issuer_failed', 0)
        self.assertEqual(set(os.listdir('/proc/self/fd')), before)

    def test_source_mutation_after_child_reaped_blocks_and_cleans(self):
        def change(index, response):
            self.bootstrap.write_bytes(self.original.replace(b'bootstrap-secret', b'changed-secret'))
            self.original = self.bootstrap.read_bytes()
            return response
        self.transform = change
        self.failed_clean('input_changed', 1)

    def test_replaced_output_inode_is_never_deleted(self):
        def replace(index, response):
            if index == 2:
                target = self.output / 'credentials.ini'
                target.unlink()
                target.write_text('replacement-must-remain')
                target.chmod(0o600)
            return response
        self.transform = replace
        result = self.run_issue()
        self.assertFalse(result['verified'])
        self.assertFalse(result['temporary_outputs_retired'])
        self.assertEqual((self.output / 'credentials.ini').read_text(), 'replacement-must-remain')

    def test_whole_deadline_prevents_next_child(self):
        with patch.object(session.time, 'monotonic', side_effect=[0, 91]):
            self.failed_clean('deadline_expired', 0)

    def test_invalid_credentials_line_never_reaches_temporary_identity(self):
        self.credentials['SessionToken'] = 'bad\ncredential_process=bad'
        self.failed_clean('invalid_profile', 2)

    def test_late_final_readback_revokes_success_and_retires_credentials(self):
        clock = [0]
        original = session.HeldFile.read
        def late(held):
            data = original(held)
            if held.path == str(self.output / 'credentials.ini'):
                clock[0] = 91
            return data
        with patch.object(session.time, 'monotonic', side_effect=lambda: clock[0]), \
                patch.object(session.HeldFile, 'read', late):
            self.failed_clean('deadline_expired', 3)

    def test_late_final_cleanup_revokes_success(self):
        clock = [0]
        original = session.Output.retire
        def late(output, *args, **kwargs):
            result = original(output, *args, **kwargs)
            clock[0] = 91
            return result
        with patch.object(session.time, 'monotonic', side_effect=lambda: clock[0]), \
                patch.object(session.Output, 'retire', late):
            result = self.run_issue()
        self.assertEqual(result['status'], 'deadline_expired')
        self.assertFalse(result['verified'])
        self.assertTrue(result['failed_publication_outputs_retired'])
        self.assertEqual({path.name for path in self.output.iterdir()}, {'failure.json'})

    def test_late_metadata_publication_retires_both_success_outputs(self):
        clock = [0]
        original = session.Output.create
        def late(output, name, data):
            original(output, name, data)
            if name == 'metadata.json':
                clock[0] = 91
        with patch.object(session.time, 'monotonic', side_effect=lambda: clock[0]), \
                patch.object(session.Output, 'create', late):
            result = self.run_issue()
        self.assertEqual(result['status'], 'deadline_expired')
        self.assertFalse(result['verified'])
        self.assertTrue(result['failed_publication_outputs_retired'])
        self.assertEqual({path.name for path in self.output.iterdir()}, {'failure.json'})

    def test_metadata_creation_failure_retires_credentials_and_records_public_facts(self):
        original = session.Output.create
        def fail(output, name, data):
            if name == 'metadata.json':
                raise OSError('private-raw-diagnostic')
            original(output, name, data)
        with patch.object(session.Output, 'create', fail):
            result = self.run_issue()
        self.assertFalse(result['verified'])
        self.assertEqual(result['status'], 'metadata_publication_failed')
        self.assertTrue(result['failed_publication_outputs_retired'])
        self.assertNotIn('private-raw-diagnostic', json.dumps(result))
        self.assertEqual({path.name for path in self.output.iterdir()}, {'failure.json'})
        self.assertEqual([fact['returncode'] for fact in result['calls']], [0, 0, 0])

    def test_metadata_fsync_failure_retires_partial_metadata_and_credentials(self):
        original = session.os.fsync
        def fail(fd):
            if os.readlink('/proc/self/fd/' + str(fd)) == str(self.output / 'metadata.json'):
                raise OSError('private-fsync-diagnostic')
            return original(fd)
        with patch.object(session.os, 'fsync', fail):
            result = self.run_issue()
        self.assertFalse(result['verified'])
        self.assertEqual(result['status'], 'metadata_publication_failed')
        self.assertTrue(result['failed_publication_outputs_retired'])
        self.assertEqual({path.name for path in self.output.iterdir()}, {'failure.json'})

    def test_metadata_failure_does_not_delete_replaced_credential_inode(self):
        original = session.Output.create
        def fail(output, name, data):
            if name == 'metadata.json':
                target = self.output / 'credentials.ini'
                target.unlink()
                target.write_text('unowned-replacement')
                target.chmod(0o600)
                raise OSError('failed')
            original(output, name, data)
        with patch.object(session.Output, 'create', fail):
            result = self.run_issue()
        self.assertFalse(result['verified'])
        self.assertFalse(result['failed_publication_outputs_retired'])
        self.assertEqual((self.output / 'credentials.ini').read_text(), 'unowned-replacement')

    def test_metadata_failure_with_unknown_readers_retains_their_exact_inputs(self):
        original_create = session.Output.create
        original_process = self.fake
        def fail_create(output, name, data):
            if name == 'metadata.json':
                raise OSError('failed')
            original_create(output, name, data)
        def unknown(*args, **kwargs):
            if len(self.calls) == 2:
                raise RuntimeError('cleanup unknown')
            return original_process(*args, **kwargs)
        with patch.object(session.Output, 'create', fail_create), \
                patch.object(session, 'bounded_process', unknown):
            result = self.run_issue()
        self.assertFalse(result['readers_reaped'])
        self.assertFalse(result['verified'])
        self.assertTrue((self.output / 'bootstrap.private').exists())
        self.assertTrue((self.output / 'credentials.ini').exists())

    def test_bootstrap_and_tool_fifos_are_nonblocking_and_launch_no_aws(self):
        original_open = session.os.open
        def checked_open(name, flags, *args, **kwargs):
            if name in ('credentials.ini', 'aws') and not flags & os.O_CREAT:
                self.assertTrue(flags & os.O_NONBLOCK)
            return original_open(name, flags, *args, **kwargs)
        for index, path in enumerate((self.bootstrap, self.aws)):
            with self.subTest(path=path):
                self.output = self.base / ('fifo' + str(index))
                original = path.read_bytes()
                mode = path.stat().st_mode & 0o777
                path.unlink()
                os.mkfifo(path, 0o600)
                with patch.object(session.os, 'open', checked_open):
                    result = session.issue(str(self.output))
                self.assertFalse(result['verified'])
                self.assertEqual(result['status'], 'unsafe_file' if index == 0 else 'unsafe_tool')
                self.assertFalse(self.calls)
                self.assertFalse((self.output / 'credentials.ini').exists())
                path.unlink()
                path.write_bytes(original)
                path.chmod(mode)
        self.assertEqual(self.bootstrap.read_bytes(), self.original)

    def test_adopt_fifo_is_nonblocking_and_refused(self):
        owned = session.Output(str(self.output))
        try:
            os.mkfifo(self.output / 'before.private', 0o600)
            original_open = session.os.open
            def checked_open(name, flags, *args, **kwargs):
                if name == 'before.private':
                    self.assertTrue(flags & os.O_NONBLOCK)
                return original_open(name, flags, *args, **kwargs)
            with patch.object(session.os, 'open', checked_open):
                with self.assertRaisesRegex(session.private.InputBlocked, 'unsafe_file'):
                    owned.adopt('before.private')
            self.assertFalse(self.calls)
        finally:
            owned.close()


if __name__ == '__main__':
    unittest.main()
