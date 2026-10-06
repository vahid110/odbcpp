from contextlib import ExitStack
from types import SimpleNamespace
import os
import stat
import traceback
import unittest
from unittest.mock import patch
from tools.redshift.native_auth_inputs import prepare_snapshot, InputBlocked, PROFILE, FILES


RAW = b'[default]\ncredential_process=never-run\n[odbcpp-redshift-pilot]\naws_access_key_id=synthetic-access\naws_secret_access_key=synthetic-secret\naws_session_token=synthetic-token\n[other]\naws_secret_access_key=not-exported\n'
ADMIN = {'host': 'fixture.invalid', 'port': 5439, 'database': 'odbcpp_pilot',
         'admin_user': 'IAM:admin', 'admin_password': 'synthetic:pass\\word'}


class FakeFs:
    def __init__(self, data=RAW):
        self.nodes, self.fds, self.offsets = {}, {}, {}
        self.next_fd, self.next_ino = 10, 1
        self.events = []
        self.write_fault = False
        self.partial_fault = False
        self.partial_started = False
        self.fsync_fault = False
        self.metadata_fault = False
        self.close_fault = False
        self.stat_hook = None
        for p in ('/', '/source', '/private'):
            self.add(p, stat.S_IFDIR | 0o700)
        self.add('/source/credentials', stat.S_IFREG | 0o600, data)

    def add(self, path, mode, data=b''):
        self.nodes[path] = {'mode': mode, 'data': bytearray(data), 'ino': self.next_ino, 'uid': 501, 'links': 1}
        self.next_ino += 1

    def path(self, name, dir_fd=None):
        if name.startswith('/'):
            return name
        return self.fds[dir_fd].rstrip('/') + '/' + name

    def info(self, path):
        n = self.nodes[path]
        return SimpleNamespace(st_dev=1, st_ino=n['ino'], st_uid=n['uid'], st_mode=n['mode'],
                               st_nlink=n['links'], st_size=len(n['data']), st_mtime_ns=0, st_ctime_ns=0)

    def open(self, name, flags, mode=0o777, *, dir_fd=None):
        path = self.path(name, dir_fd)
        self.events.append(('open', path, flags))
        if flags & os.O_CREAT:
            if path in self.nodes:
                raise FileExistsError('synthetic-private-detail')
            self.add(path, stat.S_IFREG | mode)
        if path not in self.nodes or stat.S_ISLNK(self.nodes[path]['mode']):
            raise OSError('synthetic-private-detail')
        fd = self.next_fd
        self.next_fd += 1
        self.fds[fd], self.offsets[fd] = path, 0
        return fd

    def fstat(self, fd):
        if self.metadata_fault and self.fds[fd].startswith('/private/snapshot/'):
            raise OSError('synthetic-private-metadata')
        return self.info(self.fds[fd])

    def stat(self, name, *, dir_fd=None, follow_symlinks=False):
        path = self.path(name, dir_fd)
        if self.stat_hook:
            self.stat_hook(path)
        return self.info(path)

    def read(self, fd, size):
        self.events.append(('read', self.fds[fd]))
        data = self.nodes[self.fds[fd]]['data']
        at = self.offsets[fd]
        self.offsets[fd] += min(size, len(data) - at)
        return bytes(data[at:at + size])

    def write(self, fd, payload):
        self.events.append(('write', self.fds[fd]))
        if self.write_fault and self.fds[fd].endswith('/admin.pgpass'):
            raise OSError('synthetic-private-write')
        data = bytes(payload)
        if self.partial_fault and self.fds[fd].endswith('/credentials'):
            if self.partial_started:
                raise OSError('synthetic-private-partial-write')
            self.partial_started = True
            data = data[:max(1, len(data) // 2)]
        self.nodes[self.fds[fd]]['data'].extend(data)
        return len(data)

    def mkdir(self, name, mode, *, dir_fd):
        path = self.path(name, dir_fd)
        self.events.append(('mkdir', path))
        if path in self.nodes:
            raise FileExistsError()
        self.add(path, stat.S_IFDIR | mode)

    def fsync(self, fd):
        self.events.append(('fsync', self.fds[fd]))
        if self.fsync_fault and self.fds[fd].endswith('/admin.pgpass'):
            raise OSError('synthetic-private-sync')

    def unlink(self, name, *, dir_fd):
        path = self.path(name, dir_fd)
        self.events.append(('unlink', path))
        del self.nodes[path]

    def rmdir(self, name, *, dir_fd):
        path = self.path(name, dir_fd)
        self.events.append(('rmdir', path))
        if any(p.startswith(path + '/') for p in self.nodes):
            raise OSError('not-empty')
        del self.nodes[path]

    def close(self, fd):
        self.events.append(('close', fd))
        if self.close_fault and self.fds[fd] == '/source/credentials':
            raise OSError('synthetic-private-close')
        del self.fds[fd]

    def patched(self):
        stack = ExitStack()
        for name in ('open', 'fstat', 'stat', 'read', 'write', 'mkdir', 'fsync', 'unlink', 'rmdir', 'close'):
            stack.enter_context(patch('tools.redshift.native_auth_inputs.os.' + name, getattr(self, name)))
        stack.enter_context(patch('tools.redshift.native_auth_inputs.os.geteuid', lambda: 501))
        return stack


def prepare(fs, **kwargs):
    args = dict(credentials_path='/source/credentials', profile=PROFILE,
                directory='/private/snapshot', admin=dict(ADMIN))
    args.update(kwargs)
    return prepare_snapshot(**args)


class NativeAuthInputsTests(unittest.TestCase):
    def test_selected_profile_only_private_objects_fsync_and_checked_cleanup(self):
        fs = FakeFs()
        with fs.patched():
            owner = prepare(fs)
            credentials = bytes(fs.nodes[owner.credentials_path]['data'])
            self.assertEqual(credentials, b'[odbcpp-redshift-pilot]\naws_access_key_id=synthetic-access\naws_secret_access_key=synthetic-secret\naws_session_token=synthetic-token\n')
            self.assertNotIn(b'credential_process', credentials)
            self.assertNotIn(b'not-exported', credentials)
            self.assertEqual(fs.nodes[owner.config_path]['data'], b'')
            self.assertEqual(stat.S_IMODE(fs.nodes[owner.directory]['mode']), 0o700)
            for name in FILES:
                self.assertEqual(stat.S_IMODE(fs.nodes[owner.directory + '/' + name]['mode']), 0o600)
            self.assertTrue(owner.close())
            before = list(fs.events)
            self.assertTrue(owner.close())
            self.assertEqual(fs.events, before)
        self.assertIn('/source/credentials', fs.nodes)
        self.assertNotIn('/private/snapshot', fs.nodes)
        self.assertFalse(fs.fds)

    def test_duplicate_missing_and_selected_unsupported_fields_refuse_before_create(self):
        for data in (RAW + b'[odbcpp-redshift-pilot]\n', RAW.replace(b'aws_secret_access_key=synthetic-secret', b'aws_secret_access_key='),
                     RAW.replace(b'aws_session_token=synthetic-token', b'credential_process=never'),
                     RAW.replace(b'aws_session_token=synthetic-token', b'role_arn=never'),
                     RAW.replace(b'aws_session_token=synthetic-token', b'sso_session=never'),
                     RAW.replace(b'aws_session_token=synthetic-token', b'unknown=never'),
                     RAW.replace(b'aws_session_token=synthetic-token', b'aws_access_key_id=again'),
                     b'[other]\naws_access_key_id=x\naws_secret_access_key=y\n'):
            fs = FakeFs(data)
            with fs.patched(), self.assertRaises(InputBlocked):
                prepare(fs)
            self.assertFalse(any(e[0] in ('mkdir', 'write') for e in fs.events))

    def test_source_metadata_symlink_hardlink_and_bounds_refuse_before_read_or_write(self):
        for change in ('mode', 'uid', 'links', 'symlink', 'size'):
            fs = FakeFs()
            node = fs.nodes['/source/credentials']
            if change == 'mode':
                node['mode'] = stat.S_IFREG | 0o644
            if change == 'uid':
                node['uid'] = 999
            if change == 'links':
                node['links'] = 2
            if change == 'symlink':
                node['mode'] = stat.S_IFLNK | 0o777
            if change == 'size':
                node['data'] = bytearray(b'x' * 16385)
            with fs.patched(), self.assertRaises(InputBlocked):
                prepare(fs)
            self.assertFalse(any(e[0] in ('read', 'write', 'mkdir') for e in fs.events))
        fs = FakeFs()
        with fs.patched(), self.assertRaises(InputBlocked):
            prepare(fs, profile='default')
        self.assertFalse(fs.events)

    def test_passfile_exact_tuple_escapes_colon_backslash_and_refuses_newlines(self):
        fs = FakeFs()
        with fs.patched():
            owner = prepare(fs)
            self.assertEqual(bytes(fs.nodes[owner.passfile_path]['data']),
                             b'fixture.invalid:5439:odbcpp_pilot:IAM\\:admin:synthetic\\:pass\\\\word\n')
            self.assertTrue(owner.close())
        for password in ('x\ny', 'x\ry', 'x\0y', '\ud800'):
            fs = FakeFs()
            with fs.patched(), self.assertRaises(InputBlocked):
                prepare(fs, admin=dict(ADMIN, admin_password=password))
            self.assertFalse(fs.events)

    def test_created_file_identity_checked_before_any_secret_write(self):
        fs = FakeFs()
        fs.metadata_fault = True
        with fs.patched(), self.assertRaises(InputBlocked) as caught:
            prepare(fs)
        self.assertEqual(str(caught.exception), 'cleanup_unknown')
        self.assertIsNotNone(caught.exception.residue)
        self.assertFalse(any(e[0] == 'write' for e in fs.events))
        self.assertFalse(fs.fds)
        self.assertIn('/private/snapshot/credentials', fs.nodes)  # unknown inode never guessed/unlinked

    def test_partial_write_or_sync_failure_cleans_only_known_objects(self):
        for fault in ('write_fault', 'fsync_fault', 'partial_fault'):
            fs = FakeFs()
            setattr(fs, fault, True)
            with fs.patched(), self.assertRaises(InputBlocked) as caught:
                prepare(fs)
            self.assertEqual(str(caught.exception), 'preparation_failed')
            self.assertTrue(caught.exception.__suppress_context__)
            self.assertFalse(fs.fds)
            self.assertIn('/source/credentials', fs.nodes)
            self.assertNotIn('/private/snapshot', fs.nodes)
        self.assertNotIn('synthetic-private', ''.join(traceback.format_exception(caught.exception)))

    def test_replaced_child_is_never_unlinked_and_cleanup_cannot_heal(self):
        fs = FakeFs()
        with fs.patched():
            owner = prepare(fs)
            fs.nodes[owner.credentials_path]['ino'] = 99999
            self.assertFalse(owner.close())
            before = list(fs.events)
            self.assertFalse(owner.close())
            self.assertEqual(fs.events, before)
        self.assertIn('/private/snapshot/credentials', fs.nodes)
        self.assertFalse(fs.fds)

    def test_source_drift_and_close_error_refuse_publication_with_safe_diagnostics(self):
        fs = FakeFs()
        fs.stat_hook = lambda path: fs.nodes[path].update(ino=999) if path == '/source/credentials' else None
        with fs.patched(), self.assertRaises(InputBlocked):
            prepare(fs)
        self.assertFalse(any(e[0] == 'write' for e in fs.events))
        fs = FakeFs()
        fs.close_fault = True
        with fs.patched(), self.assertRaises(InputBlocked) as caught:
            prepare(fs)
        self.assertTrue(caught.exception.__suppress_context__)
        self.assertEqual(str(caught.exception), 'cleanup_unknown')
        self.assertNotIn('synthetic-private', ''.join(traceback.format_exception(caught.exception)))
        self.assertNotIn('/private/snapshot', fs.nodes)


    def test_source_parent_walk_ambiguous_close_guards_next_and_never_retries(self):
        for consumed in (False, True):
            fs = FakeFs()
            original_close = fs.close
            def close(fd):
                if fs.fds[fd] == '/':
                    fs.events.append(('close', fd))
                    if consumed:
                        del fs.fds[fd]
                    raise OSError('synthetic-private-ambiguous-close')
                return original_close(fd)
            fs.close = close
            with fs.patched(), self.assertRaises(InputBlocked) as caught:
                prepare(fs)
            self.assertEqual(str(caught.exception), 'cleanup_unknown')
            self.assertTrue(caught.exception.__suppress_context__)
            self.assertNotIn('synthetic-private', ''.join(traceback.format_exception(caught.exception)))
            self.assertEqual([e[1] for e in fs.events if e[0] == 'close'], [10, 11])
            self.assertFalse(any(e[0] in ('read', 'write', 'mkdir') for e in fs.events))
            self.assertEqual(set(fs.fds.values()), set() if consumed else {'/'})

    def test_destination_walk_unknown_survives_successful_source_cleanup(self):
        for consumed in (False, True):
            fs = FakeFs()
            original_close = fs.close
            root_closes = 0
            def close(fd):
                nonlocal root_closes
                if fs.fds[fd] == '/':
                    root_closes += 1
                    if root_closes == 2:
                        fs.events.append(('close', fd))
                        if consumed:
                            del fs.fds[fd]
                        raise OSError('synthetic-private-ambiguous-close')
                return original_close(fd)
            fs.close = close
            with fs.patched(), self.assertRaises(InputBlocked) as caught:
                prepare(fs)
            self.assertEqual(str(caught.exception), 'cleanup_unknown')
            self.assertIsNone(caught.exception.residue)
            closes = [e[1] for e in fs.events if e[0] == 'close']
            self.assertEqual(len(closes), len(set(closes)))
            self.assertIn(14, closes)
            self.assertNotIn('/private', fs.fds.values())
            self.assertNotIn('/source', fs.fds.values())
            self.assertNotIn('/source/credentials', fs.fds.values())
            self.assertFalse(any(e[0] in ('write', 'mkdir') for e in fs.events))
            self.assertEqual(set(fs.fds.values()), set() if consumed else {'/'})

    def test_failed_walk_open_or_guard_cleanup_has_one_close_attempt_per_acquisition(self):
        fs = FakeFs()
        fs.nodes.pop('/source')
        with fs.patched(), self.assertRaises(InputBlocked) as caught:
            prepare(fs)
        self.assertEqual(str(caught.exception), 'preparation_failed')
        self.assertEqual([e for e in fs.events if e[0] == 'close'], [('close', 10)])
        self.assertFalse(fs.fds)
        fs = FakeFs()
        def close(fd):
            fs.events.append(('close', fd))
            raise OSError('synthetic-private-all-closes-ambiguous')
        fs.close = close
        with fs.patched(), self.assertRaises(InputBlocked) as caught:
            prepare(fs)
        self.assertEqual(str(caught.exception), 'cleanup_unknown')
        self.assertEqual([e for e in fs.events if e[0] == 'close'], [('close', 10), ('close', 11)])
        self.assertFalse(any(e[0] in ('read', 'write', 'mkdir') for e in fs.events))
