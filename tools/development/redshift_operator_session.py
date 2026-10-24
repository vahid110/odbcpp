"""Explicit finite WSL session issuance for the existing Redshift pilot IAM user.

Qualification tooling, not an SDK authentication provider or resource admission.
No work occurs on import. One invocation owns one new private directory; there
are no retries, timers, IAM changes, resource operations or credential discovery.
Custody checks assume a cooperating same-UID owner, not a hostile same-UID writer.
Python/CLI credential copies cannot be guaranteed erased from memory or storage.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import sys
import time

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.redshift import native_auth_inputs as private
from tools.redshift.pilot_preflight import Blocked
from tools.redshift.pilot_runtime import bounded_process

BOOTSTRAP = '/home/vahid/.local/state/odbcpp/operator-bootstrap024/credentials.ini'
AWS = '/home/vahid/.local/share/odbcpp/aws-cli-2.38.1/v2/2.38.1/dist/aws'
AWS_SHA256 = 'b95378674f18a78594c62334b2cca9363f4d5c71bd160f578a88b35d28335e09'
ACCOUNT = '385207570137'
ARN = 'arn:aws:iam::385207570137:user/odbcpp-redshift-pilot'
REGION = 'eu-north-1'
ENDPOINT = 'https://sts.eu-north-1.amazonaws.com'
SESSION_SECONDS = 43200
TOTAL_SECONDS = 90


class Refused(RuntimeError):
    """Only fixed, public reason codes may escape the issuer."""


def need(value, reason):
    if not value:
        raise Refused(reason)


def _private_directory(info):
    need(stat.S_ISDIR(info.st_mode) and info.st_uid == os.geteuid() and
         stat.S_IMODE(info.st_mode) == 0o700, 'unsafe_directory')


class HeldFile:
    """Reuse the existing qualification NOFOLLOW/owner/mode/link/readback rules."""
    def __init__(self, path, *, executable=False):
        self.parent = self.fd = None
        self.path = path
        self.executable = executable
        try:
            parts = private._parts(path)
            self.parent = private._directory(parts[:-1])
            self.name = parts[-1]
            if not executable:
                _private_directory(os.fstat(self.parent))
            self.fd = os.open(self.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK,
                              dir_fd=self.parent)
            info = os.fstat(self.fd)
            if executable:
                need(stat.S_ISREG(info.st_mode) and info.st_uid in (0, os.geteuid()) and
                     not info.st_mode & 0o022 and info.st_nlink == 1 and
                     info.st_mode & 0o100 and 0 < info.st_size <= 64 * 1024 * 1024,
                     'unsafe_tool')
            else:
                private._regular(info)
            self.stamp = private._stamp(info)
            self.check()
        except Exception:
            self.close()
            raise

    def check(self):
        if not self.executable:
            _private_directory(os.fstat(self.parent))
        current_parent = private._directory(private._parts(self.path)[:-1])
        try:
            need(private._stamp(os.fstat(current_parent))[:2] ==
                 private._stamp(os.fstat(self.parent))[:2], 'input_changed')
        finally:
            os.close(current_parent)
        need(private._stamp(os.fstat(self.fd)) == self.stamp and
             private._stamp(os.stat(self.name, dir_fd=self.parent,
                                    follow_symlinks=False)) == self.stamp,
             'input_changed')

    def read(self):
        self.check()
        os.lseek(self.fd, 0, os.SEEK_SET)
        data = os.read(self.fd, private.LIMIT + 1)
        need(len(data) == self.stamp[5] and len(data) <= private.LIMIT, 'invalid_output')
        self.check()
        return data

    def digest(self):
        self.check()
        os.lseek(self.fd, 0, os.SEEK_SET)
        digest = hashlib.sha256()
        while data := os.read(self.fd, 1024 * 1024):
            digest.update(data)
        self.check()
        return digest.hexdigest()

    def close(self):
        clean = True
        for attr in ('fd', 'parent'):
            fd = getattr(self, attr, None)
            setattr(self, attr, None)
            if fd is not None:
                try:
                    os.close(fd)
                except Exception:
                    # A failed close may already have consumed the descriptor.
                    # Never retry it; still attempt the other acquisition once.
                    clean = False
        need(clean, 'input_close_failed')


class Output:
    def __init__(self, directory):
        self.path, self.parent, self.fd = directory, None, None
        self.files = {}
        self.retired = set()
        try:
            parts = private._parts(directory)
            self.name = parts[-1]
            self.parent = private._directory(parts[:-1])
            _private_directory(os.fstat(self.parent))
            # An existing directory is never adopted, even if apparently empty.
            os.mkdir(self.name, 0o700, dir_fd=self.parent)
            self.fd = os.open(self.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW |
                              os.O_CLOEXEC, dir_fd=self.parent)
            _private_directory(os.fstat(self.fd))
            self.identity = private._stamp(os.fstat(self.fd))[:2]
            os.fsync(self.parent)
        except Exception:
            self.close()
            raise

    def check(self):
        _private_directory(os.fstat(self.parent))
        current_parent = private._directory(private._parts(self.path)[:-1])
        try:
            need(private._stamp(os.fstat(current_parent))[:2] ==
                 private._stamp(os.fstat(self.parent))[:2], 'output_directory_changed')
        finally:
            os.close(current_parent)
        _private_directory(os.fstat(self.fd))
        info = os.stat(self.name, dir_fd=self.parent, follow_symlinks=False)
        need(private._stamp(info)[:2] == self.identity, 'output_directory_changed')
        _private_directory(info)

    def create(self, name, data):
        self.check()
        fd = os.open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW |
                     os.O_CLOEXEC, 0o600, dir_fd=self.fd)
        # Own the acquisition immediately, including interrupted writes.
        self.files[name] = (fd, private._stamp(os.fstat(fd))[:2])
        os.fchmod(fd, 0o600)
        view = memoryview(data)
        while view:
            written = os.write(fd, view)
            need(written > 0, 'write_failed')
            view = view[written:]
        os.fsync(fd)
        private._regular(os.fstat(fd))
        os.fsync(self.fd)

    def adopt(self, name):
        self.check()
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK,
                     dir_fd=self.fd)
        try:
            info = os.fstat(fd)
            # Track this exact inode before parsing/size validation can fail.
            self.files[name] = (fd, private._stamp(info)[:2])
            private._regular(info)
            need(private._stamp(os.stat(name, dir_fd=self.fd, follow_symlinks=False)) ==
                 private._stamp(info), 'output_changed')
            data = os.read(fd, private.LIMIT + 1)
            need(len(data) == info.st_size and private._stamp(os.fstat(fd)) ==
                 private._stamp(info), 'output_changed')
            return data
        except Exception:
            if name not in self.files:
                os.close(fd)
            raise

    def retire(self, keep=(), *, only=None):
        self.check()
        clean = True
        for name, (fd, identity) in list(self.files.items()):
            if name in keep or name in self.retired or (only is not None and name not in only):
                continue
            try:
                info = os.stat(name, dir_fd=self.fd, follow_symlinks=False)
                # A reaped child may have exceeded the accepted response size.
                # Size never permits deleting an unowned/replaced/linked inode.
                need(stat.S_ISREG(info.st_mode) and info.st_uid == os.geteuid() and
                     stat.S_IMODE(info.st_mode) == 0o600 and info.st_nlink == 1,
                     'unsafe_file')
                need(private._stamp(info)[:2] == identity and
                     private._stamp(os.fstat(fd))[:2] == identity, 'output_changed')
                os.unlink(name, dir_fd=self.fd)
                self.retired.add(name)
            except Exception:
                clean = False
        os.fsync(self.fd)
        return clean

    def close(self):
        for fd, _ in self.files.values():
            os.close(fd)
        self.files.clear()
        for attr in ('fd', 'parent'):
            fd = getattr(self, attr, None)
            setattr(self, attr, None)
            if fd is not None:
                os.close(fd)


def _json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            need(key not in result, 'invalid_response')
            result[key] = value
        return result
    try:
        value = json.loads(data, object_pairs_hook=pairs,
                           parse_constant=lambda _: (_ for _ in ()).throw(Refused('invalid_response')))
        need(type(value) is dict, 'invalid_response')
        return value
    except (UnicodeError, ValueError):
        raise Refused('invalid_response') from None


def _bootstrap(data):
    selected = private._selected(data, private.PROFILE)
    # The bootstrap is one minimal static profile, not a credential provider chain.
    try:
        text = data.decode('ascii')
    except UnicodeError:
        raise Refused('invalid_bootstrap') from None
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    need(len(lines) == 3 and lines[0] == '[' + private.PROFILE + ']' and
         {line.split('=', 1)[0].strip() for line in lines[1:]} ==
         {'aws_access_key_id', 'aws_secret_access_key'} and
         b'aws_session_token' not in selected, 'invalid_bootstrap')
    access = next(line.split('=', 1)[1].strip() for line in lines[1:]
                  if line.split('=', 1)[0].strip() == 'aws_access_key_id')
    need(re.fullmatch(r'AKIA[A-Z0-9]{16}', access), 'invalid_bootstrap')
    return selected


def _identity(data):
    value = _json(data)
    need(set(value) == {'Account', 'Arn', 'UserId'} and value['Account'] == ACCOUNT and
         value['Arn'] == ARN and type(value['UserId']) is str and
         re.fullmatch(r'AIDA[A-Z0-9]{12,32}', value['UserId']), 'wrong_identity')
    return value


def _credentials(data, now, started):
    response = _json(data)
    need(set(response) <= {'Credentials', 'SessionTokenSize', 'SessionTokenUtilization'} and
         'Credentials' in response, 'invalid_response')
    value = response['Credentials']
    need(type(value) is dict and set(value) ==
         {'AccessKeyId', 'SecretAccessKey', 'SessionToken', 'Expiration'}, 'invalid_response')
    need(all(type(v) is str for v in value.values()), 'invalid_response')
    try:
        expiry = datetime.fromisoformat(value['Expiration'].replace('Z', '+00:00'))
        need(expiry.utcoffset() is not None and expiry.utcoffset().total_seconds() == 0,
             'invalid_expiry')
        # The observed result must match the fixed twelve-hour request within
        # the existing finite issuance window, not merely be unexpired.
        need(now < expiry and SESSION_SECONDS - TOTAL_SECONDS <=
             (expiry - started).total_seconds() <= SESSION_SECONDS + TOTAL_SECONDS,
             'invalid_expiry')
    except (ValueError, TypeError):
        raise Refused('invalid_expiry') from None
    need(re.fullmatch(r'ASIA[A-Z0-9]{16}', value['AccessKeyId']), 'invalid_credentials')
    data = ('[' + private.PROFILE + ']\naws_access_key_id=' + value['AccessKeyId'] +
            '\naws_secret_access_key=' + value['SecretAccessKey'] +
            '\naws_session_token=' + value['SessionToken'] + '\n').encode('ascii')
    return private._selected(data, private.PROFILE), expiry.isoformat().replace('+00:00', 'Z')


def _environment(directory, credentials):
    # Construct from scratch: no inherited keys, endpoints, CA overrides, proxies,
    # web identity, container credentials, SSO, process provider, or config sources.
    return {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C', 'HOME': directory,
            'AWS_SHARED_CREDENTIALS_FILE': directory + '/' + credentials,
            'AWS_CONFIG_FILE': directory + '/config.private', 'AWS_PROFILE': private.PROFILE,
            'AWS_DEFAULT_REGION': REGION, 'AWS_REGION': REGION,
            'AWS_EC2_METADATA_DISABLED': 'true', 'AWS_MAX_ATTEMPTS': '1',
            'AWS_RETRY_MODE': 'standard', 'AWS_PAGER': '', 'AWS_CLI_AUTO_PROMPT': 'off'}


def _finish(output, metadata, success, reason, deadline):
    """Success includes final retirement and durable metadata within its deadline.

    Failure reporting/checked retirement may continue after expiry, but can never
    publish or return an accepted credential. Publication failure gets one fixed
    failure record; no retry of issuance, or of an ambiguous publication inode.
    """
    try:
        if metadata['readers_reaped']:
            clean = output.retire(('credentials.ini',) if success else ())
            metadata['temporary_outputs_retired'] = clean
            if not clean:
                success, reason = False, 'cleanup_unknown'
                output.retire(only=('credentials.ini',))
        else:
            success, reason = False, 'process_cleanup_unknown'
            metadata['temporary_outputs_retired'] = False
        if success:
            need(time.monotonic() < deadline, 'deadline_expired')
        metadata.update({'verified': success, 'status': reason,
                         'finished_at': datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z')})
        output.create('metadata.json', (json.dumps(metadata, sort_keys=True, indent=2) + '\n').encode('ascii'))
        if success:
            need(time.monotonic() < deadline, 'deadline_expired')
    except Exception as error:
        metadata.update({'verified': False, 'status': 'deadline_expired' if
                         isinstance(error, Refused) and str(error) == 'deadline_expired'
                         else 'metadata_publication_failed',
                         'finished_at': datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z')})
        if metadata['readers_reaped']:
            try:
                clean = output.retire(only=('credentials.ini', 'metadata.json'))
            except Exception:
                clean = False
            metadata['failed_publication_outputs_retired'] = clean
        # A partial/late successful metadata inode is retired above, never
        # rewritten or followed. This distinct record contains public facts only.
        metadata['failure_metadata_file'] = 'failure.json'
        try:
            output.create('failure.json', (json.dumps(metadata, sort_keys=True, indent=2) + '\n').encode('ascii'))
        except Exception:
            # Persistent storage itself may have failed. CLI still returns these
            # sanitized failure facts and can never return an accepted session.
            metadata['failure_metadata_file'] = None
    return metadata


def issue(directory):
    """Return only public metadata. A failure never retries or reuses its directory."""
    need(os.name == 'posix', 'posix_required')
    started = datetime.now(timezone.utc)
    deadline = time.monotonic() + TOTAL_SECONDS
    output = Output(directory)
    source = tool = None
    metadata = {'schema': 1, 'verified': False, 'status': 'failed',
                'account': ACCOUNT, 'arn': ARN, 'region': REGION, 'endpoint': ENDPOINT,
                'requested_seconds': SESSION_SECONDS, 'bootstrap_path': BOOTSTRAP,
                'tool_path': AWS, 'tool_sha256': AWS_SHA256,
                'issuer_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'started_at': started.isoformat().replace('+00:00', 'Z'),
                'aws_calls_started': 0, 'readers_reaped': True, 'calls': []}
    success, reason = False, 'issuer_failed'
    try:
        source = HeldFile(BOOTSTRAP)
        tool = HeldFile(AWS, executable=True)
        need(tool.digest() == AWS_SHA256, 'tool_changed')
        output.create('bootstrap.private', _bootstrap(source.read()))
        output.create('config.private', b'')

        def call(operation, credentials, name, extra=()):
            output.check()
            source.check()
            need(tool.digest() == AWS_SHA256, 'tool_changed')
            remaining = deadline - time.monotonic()
            need(remaining >= 6, 'deadline_expired')
            argv = [AWS, 'sts', operation, *extra, '--profile', private.PROFILE,
                    '--region', REGION, '--endpoint-url', ENDPOINT, '--output', 'json',
                    '--no-cli-pager', '--no-cli-auto-prompt',
                    '--cli-connect-timeout', '5', '--cli-read-timeout', '5']
            metadata['aws_calls_started'] += 1
            metadata['readers_reaped'] = False
            observation = {'operation': operation, 'returncode': None,
                           'timed_out': None, 'readers_reaped': False}
            metadata['calls'].append(observation)
            # bounded_process returns only after the entire child group is reaped.
            result = bounded_process(argv, env=_environment(directory, credentials),
                                     seconds=min(20, math.floor(remaining - 5)),
                                     output=Path(directory) / name)
            metadata['readers_reaped'] = True
            # Preserve the actual supervised result before parsing/retiring raw
            # output; no exception text or AWS response becomes a diagnostic.
            observation.update({'returncode': result.returncode,
                                'timed_out': result.timed_out, 'readers_reaped': True})
            data = output.adopt(name)
            need(not result.timed_out and result.returncode == 0, 'aws_call_failed')
            need(time.monotonic() < deadline, 'deadline_expired')
            source.check()
            need(tool.digest() == AWS_SHA256, 'tool_changed')
            return data

        before = _identity(call('get-caller-identity', 'bootstrap.private', 'before.private'))
        credentials, expiration = _credentials(call('get-session-token', 'bootstrap.private',
            'session.private', ('--duration-seconds', str(SESSION_SECONDS))),
            datetime.now(timezone.utc), started)
        output.create('credentials.ini', credentials)
        after = _identity(call('get-caller-identity', 'credentials.ini', 'after.private'))
        need(before == after, 'identity_changed')
        # Read back the exact published inode after its CLI reader has been reaped.
        with_file = HeldFile(directory + '/credentials.ini')
        try:
            need(with_file.read() == credentials, 'output_changed')
        finally:
            with_file.close()
        need(time.monotonic() < deadline, 'deadline_expired')
        need(datetime.fromisoformat(expiration.replace('Z', '+00:00')) >
             datetime.now(timezone.utc), 'invalid_expiry')
        metadata.update({'identity': after, 'expires_at': expiration})
        success = True
        reason = 'verified'
    except (Refused, private.InputBlocked, Blocked) as error:
        reason = str(error)
    except Exception:
        # Exception messages, raw CLI output and credentials never leave custody.
        reason = 'issuer_failed'
    finally:
        try:
            # Close non-output acquisitions before the final acceptance check.
            close_failures = []
            for label, held in (('tool', tool), ('bootstrap', source)):
                if held is not None:
                    try:
                        held.close()
                    except Exception:
                        close_failures.append(label)
            metadata['input_close_failures'] = close_failures
            if close_failures:
                success, reason = False, 'input_close_failed'
            _finish(output, metadata, success, reason, deadline)
        finally:
            output.close()
    return metadata


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-directory', required=True,
                        help='new absolute directory under an existing owner-only 0700 parent')
    args = parser.parse_args(argv)
    try:
        result = issue(args.output_directory)
        print(json.dumps(result, sort_keys=True))
        return 0 if result['verified'] else 1
    except Exception:
        print(json.dumps({'verified': False, 'status': 'output_unavailable'}))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
