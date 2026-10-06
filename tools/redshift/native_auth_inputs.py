"""One explicitly authorized root-owned static-profile/admin input snapshot.

No source discovery, provider/config loading, processes, cloud calls or admission.
Caller selects the exact new directory before calling, owns its protected namespace
and no-mutator lifetime, and records it externally before effects. Caller retains
this owner until all native/psql readers are reaped/quiescent, then closes it.
Checked cleanup is an observation, not arbitrary same-UID race protection or
crash-erasure proof. Python immutable string/bytes copies cannot be fully wiped.
"""
import os
import re
import stat

from .native_auth_cleanup import DATABASE, _PRINCIPAL

PROFILE = 'odbcpp-redshift-pilot'
LIMIT = 16384
FILES = ('credentials', 'config', 'admin.pgpass')
_KEYS = ('aws_access_key_id', 'aws_secret_access_key', 'aws_session_token')
_ATOM = re.compile(r'[A-Za-z0-9_.-]{1,256}', re.ASCII)


class InputBlocked(RuntimeError):
    def __init__(self, reason, residue=None):
        super().__init__(reason)
        self.residue = residue  # owner of known public filenames only, never secrets


def _parts(path):
    if type(path) is not str or not path.startswith('/') or len(path) > 4096:
        raise InputBlocked('invalid_path') from None
    parts = path[1:].split('/')
    if not parts or any(not _ATOM.fullmatch(p) or p in ('.', '..') for p in parts):
        raise InputBlocked('invalid_path') from None
    return parts


def _directory(parts):
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    # Transfer ownership immediately, before attempting the previous close.
    # A failed close is ambiguous; its consumed attempt is never retried.
    close_unknown = False
    try:
        for part in parts:
            next_fd = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
            previous_fd, fd = fd, next_fd
            try:
                os.close(previous_fd)
            except Exception:
                close_unknown = True
                raise
        return fd
    except Exception:
        try:
            os.close(fd)  # sole surviving acquisition, exactly one attempt
        except Exception:
            close_unknown = True
        raise InputBlocked('cleanup_unknown' if close_unknown else 'source_unavailable') from None


def _stamp(info):
    return (info.st_dev, info.st_ino, info.st_uid, info.st_mode, info.st_nlink,
            info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _regular(info, *, empty=False):
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or
            stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1 or
            not 0 <= info.st_size <= LIMIT or (empty and info.st_size != 0)):
        raise InputBlocked('unsafe_file') from None


def _selected(data, profile):
    if profile != PROFILE or not 0 < len(data) <= LIMIT:
        raise InputBlocked('invalid_profile') from None
    try:
        text = data.decode('ascii')
    except UnicodeError:
        raise InputBlocked('invalid_profile') from None
    if any(ord(c) < 32 and c not in '\n\r\t' or ord(c) == 127 for c in text):
        raise InputBlocked('invalid_profile') from None
    section, sections, selected = None, set(), {}
    lines = text.split('\n')
    if len(lines) > 256:
        raise InputBlocked('invalid_profile') from None
    for raw in lines:
        if len(raw) > 4096 or '\r' in raw.removesuffix('\r'):
            raise InputBlocked('invalid_profile') from None
        line = raw.removesuffix('\r').strip(' \t')
        if not line or line.startswith(('#', ';')):
            continue
        if line.startswith('['):
            if not line.endswith(']') or not _ATOM.fullmatch(line[1:-1]) or line[1:-1] in sections:
                raise InputBlocked('invalid_profile') from None
            section = line[1:-1]
            sections.add(section)
            continue
        if section is None or '=' not in line:
            raise InputBlocked('invalid_profile') from None
        if section != profile:
            continue  # never execute/export/load another section's source directives
        key, value = (p.strip(' \t') for p in line.split('=', 1))
        if (key not in _KEYS or key in selected or not 1 <= len(value) <= 2048 or
                any(not 33 <= ord(c) <= 126 or c in '#;\"\'' for c in value)):
            raise InputBlocked('invalid_profile') from None
        selected[key] = value
    if profile not in sections or not all(k in selected for k in _KEYS[:2]):
        raise InputBlocked('invalid_profile') from None
    return bytearray(('[' + profile + ']\n' + ''.join(k + '=' + selected[k] + '\n'
                    for k in _KEYS if k in selected)).encode('ascii'))


def _passfile(admin):
    if type(admin) is not dict or set(admin) != {'host', 'port', 'database', 'admin_user', 'admin_password'}:
        raise InputBlocked('invalid_admin') from None
    host, user, password = admin['host'], admin['admin_user'], admin['admin_password']
    if (type(host) is not str or not 1 <= len(host) <= 253 or any(
            not re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?', part) for part in host.split('.')) or
            type(admin['port']) is not int or admin['port'] != 5439 or
            type(admin['database']) is not str or admin['database'] != DATABASE or
            type(user) is not str or not _PRINCIPAL.fullmatch(user) or
            type(password) is not str or not 1 <= len(password) <= 4096 or
            any(c in password for c in '\x00\r\n')):
        raise InputBlocked('invalid_admin') from None
    escape = lambda value: value.replace('\\', '\\\\').replace(':', '\\:')
    try:
        payload = bytearray((':'.join(escape(v) for v in (host, '5439', DATABASE, user, password)) + '\n').encode('utf-8'))
    except UnicodeError:
        raise InputBlocked('invalid_admin') from None
    if len(payload) > LIMIT:
        raise InputBlocked('invalid_admin') from None
    return payload


class _InputSnapshot:
    """Known root-owned filenames/fds only; explicit one-attempt close after use."""
    def __init__(self, directory, parent_fd, leaf):
        self.directory = directory
        self.credentials_path = directory + '/credentials'
        self.config_path = directory + '/config'
        self.passfile_path = directory + '/admin.pgpass'
        self._parent, self._leaf, self._directory = parent_fd, leaf, None
        self._directory_stamp = None
        self._files = {}
        self._closed, self._busy, self._clean, self._fault = False, False, False, False

    def close(self):
        if self._busy:
            self._fault = True
            return False
        if self._closed:
            return self._clean
        self._busy, self._closed = True, True  # retire BEFORE syscall callbacks
        clean = True
        try:
            for name, (fd, stamp) in self._files.items():
                try:
                    current = os.stat(name, dir_fd=self._directory, follow_symlinks=False)
                    _regular(current)
                    if stamp is None or current.st_dev != stamp[0] or current.st_ino != stamp[1]:
                        clean = False
                    else:
                        os.unlink(name, dir_fd=self._directory)
                except Exception:
                    clean = False
                finally:
                    try:
                        os.close(fd)
                    except Exception:
                        clean = False
            if self._directory is not None:
                try:
                    os.fsync(self._directory)
                    current = os.stat(self._leaf, dir_fd=self._parent, follow_symlinks=False)
                    if (current.st_dev, current.st_ino) != self._directory_stamp[:2]:
                        clean = False
                    else:
                        os.rmdir(self._leaf, dir_fd=self._parent)
                        os.fsync(self._parent)
                except Exception:
                    clean = False
                finally:
                    try:
                        os.close(self._directory)
                    except Exception:
                        clean = False
            else:
                clean = False  # directory identity could not be established; never guess/delete
        finally:
            try:
                os.close(self._parent)
            except Exception:
                clean = False
            self._busy = False
            self._clean = clean and not self._fault
        return self._clean


def prepare_snapshot(*, credentials_path, profile, directory, admin):
    """Effects require root's separate input-preparation authorization; no CLI."""
    source_parts, destination = _parts(credentials_path), _parts(directory)
    if profile != PROFILE:
        raise InputBlocked('invalid_profile') from None
    passfile = _passfile(admin)
    source = selected = None
    source_parent = source_fd = parent = None
    owner = None
    try:
        source_parent = _directory(source_parts[:-1])
        source_fd = os.open(source_parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK, dir_fd=source_parent)
        before = os.fstat(source_fd)
        _regular(before)
        source = bytearray()
        while len(source) <= LIMIT:
            chunk = os.read(source_fd, LIMIT + 1 - len(source))
            if not chunk:
                break
            source.extend(chunk)
        named = os.stat(source_parts[-1], dir_fd=source_parent, follow_symlinks=False)
        if (len(source) != before.st_size or _stamp(os.fstat(source_fd)) != _stamp(before) or
                _stamp(named) != _stamp(before)):
            raise InputBlocked('source_changed') from None
        selected = _selected(source, profile)
        parent = _directory(destination[:-1])
        info = os.fstat(parent)
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
            raise InputBlocked('unsafe_parent') from None
        owner = _InputSnapshot(directory, parent, destination[-1])
        parent = None
        os.mkdir(owner._leaf, 0o700, dir_fd=owner._parent)
        owner._directory = os.open(owner._leaf, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=owner._parent)
        info = os.fstat(owner._directory)
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
            raise InputBlocked('unsafe_directory') from None
        owner._directory_stamp = _stamp(info)
        if _stamp(os.stat(owner._leaf, dir_fd=owner._parent, follow_symlinks=False)) != owner._directory_stamp:
            raise InputBlocked('directory_changed') from None
        os.fsync(owner._parent)
        for name, payload in zip(FILES, (selected, bytearray(), passfile)):
            fd = os.open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=owner._directory)
            # Record ownership immediately, before metadata check or secret write.
            owner._files[name] = (fd, None)
            info = os.fstat(fd)
            owner._files[name] = (fd, _stamp(info))
            _regular(info, empty=True)
            if _stamp(os.stat(name, dir_fd=owner._directory, follow_symlinks=False)) != _stamp(info):
                raise InputBlocked('file_changed') from None
            at = 0
            while at < len(payload):
                count = os.write(fd, memoryview(payload)[at:])
                if type(count) is not int or not 0 < count <= len(payload) - at:
                    raise InputBlocked('write_failed') from None
                at += count
            os.fsync(fd)
            current = os.fstat(fd)
            _regular(current)
            if current.st_size != len(payload) or _stamp(os.stat(name, dir_fd=owner._directory, follow_symlinks=False)) != _stamp(current):
                raise InputBlocked('file_changed') from None
            owner._files[name] = (fd, _stamp(current))
        os.fsync(owner._directory)
        current = os.stat(directory, follow_symlinks=False)
        if ((current.st_dev, current.st_ino) != owner._directory_stamp[:2] or
                not stat.S_ISDIR(current.st_mode) or current.st_uid != os.geteuid() or
                stat.S_IMODE(current.st_mode) != 0o700):
            raise InputBlocked('directory_changed') from None
        return owner
    except Exception as error:
        cleaned = owner.close() if owner is not None else True
        # Directory failure may precede assignment to source_parent/parent.
        unknown = not cleaned or (type(error) is InputBlocked and str(error) == 'cleanup_unknown')
        raise InputBlocked('cleanup_unknown' if unknown else 'preparation_failed', owner if unknown else None) from None
    finally:
        close_failed = False
        for fd in (source_fd, source_parent, parent):
            if fd is not None:
                try:
                    os.close(fd)
                except Exception:
                    close_failed = True
        for payload in (source, selected, passfile):
            if payload is not None:
                payload[:] = b'\x00' * len(payload)
        if close_failed:
            if owner is not None:
                owner.close()
            raise InputBlocked('cleanup_unknown', owner) from None
