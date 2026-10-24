"""Held POSIX profile inputs extracted from accepted source089; no launcher chain."""
import os
import stat
import re
import hashlib
import time
from pathlib import Path
from tools.development.redshift_ctest_lifecycle import Refused, need
def monotonic():
    return time.clock_gettime(time.CLOCK_MONOTONIC)
def _prepare_private_dsn(directory, documents):
    os.mkdir(directory, 448)
    info = directory.lstat()
    state = {'directory': str(directory), 'dev': info.st_dev, 'inode': info.st_ino, 'files': []}
    for leaf, text in zip(('odbc.ini', 'odbcinst.ini'), documents):
        path = directory / leaf
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 384)
        try:
            body = text.encode('utf-8')
            view = memoryview(body)
            while view:
                n = os.write(fd, view)
                need(n > 0, 'dsn_short_write')
                view = view[n:]
            os.fsync(fd)
            meta = os.fstat(fd)
            state['files'].append({'path': str(path), 'dev': meta.st_dev, 'inode': meta.st_ino, 'body': body})
        finally:
            os.close(fd)
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    _validate_private_dsn(state)
    return state

def _validate_private_dsn(state):
    if 'profiles' in state:
        info = Path(state['directory']).lstat()
        need(stat.S_ISDIR(info.st_mode) and info.st_dev == state['dev'] and (info.st_ino == state['inode']) and (stat.S_IMODE(info.st_mode) == 448), 'dsn_root_changed')
        need(set((p.name for p in Path(state['directory']).iterdir())) == set(state['profiles']), 'dsn_untracked_inputs')
        for value in state['profiles'].values():
            _validate_private_dsn(value)
        return
    directory = Path(state['directory'])
    info = directory.lstat()
    need(stat.S_ISDIR(info.st_mode) and info.st_uid == os.geteuid() and (stat.S_IMODE(info.st_mode) == 448) and (info.st_dev == state['dev']) and (info.st_ino == state['inode']), 'private_dsn_parent_changed')
    need(len(state['files']) == 2, 'private_dsn_incomplete')
    for row in state['files']:
        path = Path(row['path'])
        need(path.parent == directory, 'private_dsn_escape')
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            st = os.fstat(fd)
            need(stat.S_ISREG(st.st_mode) and st.st_uid == os.geteuid() and (stat.S_IMODE(st.st_mode) == 384) and (st.st_nlink == 1) and (st.st_dev == row['dev']) and (st.st_ino == row['inode']) and (st.st_size == len(row['body']) <= 32768) and (os.read(fd, 32769) == row['body']), 'private_dsn_changed')
        finally:
            os.close(fd)

def _close_private_dsn(state):
    _validate_private_dsn(state)
    if 'profiles' in state:
        for value in state['profiles'].values():
            need(_close_private_dsn(value), 'dsn_profile_cleanup')
        Path(state['directory']).rmdir()
        return not os.path.lexists(state['directory'])
    for row in state['files']:
        Path(row['path']).unlink()
    Path(state['directory']).rmdir()
    return not os.path.lexists(state['directory'])

class ProfileInputBlocked(Refused):

    def __init__(self, state):
        super().__init__('profile_private_input_preparation_failed')
        self.residue = state

PRIVATE_ENV_KEYS = frozenset(('HOME', 'SYSODBCINI', 'SYSODBCINSTINI', 'ODBCINI', 'ODBCSYSINI', 'ODBCINSTINI', 'ODBCPP_REDSHIFT_PACKAGE_CONNECTION_FILE', 'ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER', 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_HOST', 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_CA_FILE', 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_DATABASE', 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_PRINCIPAL', 'ODBCPP_REDSHIFT_AUTH_INVALID_CONNECTION', 'ODBCPP_REDSHIFT_AUTH_EXPECTED_USER'))

def _profile_input_deadline(deadline):
    if deadline is not None:
        need(type(deadline) in (int, float) and deadline == deadline and (monotonic() < deadline), 'private_input_deadline')

def _profile_directory(path, identity):
    info = path.lstat()
    need(not path.is_symlink() and stat.S_ISDIR(info.st_mode) and (info.st_uid == os.geteuid()) and (stat.S_IMODE(info.st_mode) == 448) and ((info.st_dev, info.st_ino) == tuple(identity)), 'profile_input_parent_changed')

def _profile_held_file(row, *, secret=True, deadline=None):
    _profile_input_deadline(deadline)
    path = Path(row['path'])
    info = path.lstat()
    held = os.fstat(row['fd'])
    need(not path.is_symlink() and str(path.resolve()) == row['path'] and stat.S_ISREG(info.st_mode) and ((info.st_dev, info.st_ino, info.st_size) == (held.st_dev, held.st_ino, held.st_size) == tuple(row['identity'])), 'profile_held_identity_changed')
    if secret:
        need(info.st_uid == os.geteuid() and held.st_uid == os.geteuid() and (stat.S_IMODE(info.st_mode) == stat.S_IMODE(held.st_mode) == 384) and (info.st_nlink == held.st_nlink == 1), 'profile_private_mode_changed')
        need(os.pread(row['fd'], len(row['body']) + 1, 0) == row['body'], 'profile_private_bytes_changed')
    else:
        need(info.st_nlink >= 1 and info.st_size <= 128 * 1024 * 1024, 'profile_driver_bound')
        digestor = hashlib.sha256()
        offset = 0
        while offset < info.st_size:
            _profile_input_deadline(deadline)
            block = os.pread(row['fd'], min(1024 * 1024, info.st_size - offset), offset)
            need(bool(block), 'profile_driver_short_read')
            digestor.update(block)
            offset += len(block)
        need(digestor.hexdigest() == row['sha256'], 'profile_driver_changed')

def prepare_profile_inputs(directory, config, factory, profiles, manifest, *, deadline=None):
    _profile_input_deadline(deadline)
    directory = Path(directory)
    need(directory.is_absolute() and '..' not in directory.parts and (str(directory.parent.resolve()) == str(directory.parent)), 'profile_input_path')
    os.mkdir(directory, 448)
    info = directory.lstat()
    state = {'directory': str(directory), 'identity': (info.st_dev, info.st_ino), 'profiles': {}, 'closed': False}
    try:
        for key, profile in profiles.items():
            _profile_input_deadline(deadline)
            factory.validate_profile(profile)
            need(type(key) is str and re.fullmatch('[A-Za-z0-9_-]{1,64}', key) is not None, 'profile_input_identifier')
            child = directory / key
            os.mkdir(child, 448)
            meta = child.lstat()
            part = {'directory': str(child), 'identity': (meta.st_dev, meta.st_ino), 'files': [], 'dsn': None, 'driver': None, 'ca': None, 'environment': {}}
            state['profiles'][key] = part
            private = profile['private_input']
            driver = private['driver']
            local_config = dict(config)
            if private['kind'] in ('OWN_DM_FILE', 'VENDOR_DM_FILE'):
                ca = Path(config['ca']).resolve(strict=True)
                need(ca.is_file() and type(manifest.get('ca_sha256')) is str, 'profile_canonical_CA_pending')
                fd = os.open(ca, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
                meta = os.fstat(fd)
                part['ca'] = {'path': str(ca), 'fd': fd, 'identity': (meta.st_dev, meta.st_ino, meta.st_size), 'sha256': manifest['ca_sha256']}
                _profile_held_file(part['ca'], secret=False)
                local_config['ca'] = str(ca)
            if driver is not None:
                need(manifest['pins'].get(driver['path']) == driver['sha256'], 'per_profile_driver_unbound')
                path = Path(driver['path'])
                fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
                meta = os.fstat(fd)
                part['driver'] = {'path': str(path), 'fd': fd, 'identity': (meta.st_dev, meta.st_ino, meta.st_size), 'sha256': driver['sha256']}
                _profile_held_file(part['driver'], secret=False)

            def owned(name, text):
                _profile_input_deadline(deadline)
                body = text.encode('utf-8')
                need(0 < len(body) <= 16384 and b'\x00' not in body and (b'\r' not in body) and (b'\n' not in body), 'private_connection_bytes')
                path = child / name
                fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 384)
                row = {'path': str(path), 'fd': fd, 'identity': None, 'body': body}
                part['files'].append(row)
                view = memoryview(body)
                while view:
                    _profile_input_deadline(deadline)
                    count = os.write(fd, view)
                    need(count > 0, 'private_connection_short_write')
                    view = view[count:]
                os.fsync(fd)
                held = os.fstat(fd)
                row['identity'] = (held.st_dev, held.st_ino, held.st_size)
                return str(path)
            connection = factory.build_profile_connection(local_config, profile)
            connection_file = owned('connection.txt', connection)
            if private['kind'] == 'OWN_DSN':
                manager = profile.get('driver_manager')
                part['dsn'] = _prepare_private_dsn(child / 'dsn', factory.dsn_documents(config, profile['limits'], driver['path'], driver_manager=manager))
                # unixODBC resolves ODBCINSTINI relative to ODBCSYSINI; iODBC
                # and the in-process driver take it as a path.
                part['environment'].update({'ODBCINI': str(child / 'dsn' / 'odbc.ini'), 'ODBCSYSINI': str(child / 'dsn'), 'ODBCINSTINI': 'odbcinst.ini' if manager == 'unixODBC' else str(child / 'dsn' / 'odbcinst.ini')})
            if private['kind'] in ('OWN_DM_FILE', 'VENDOR_DM_FILE'):
                if driver['wchar_bytes'] == 4:
                    part['dsn'] = _prepare_private_dsn(child / 'dsn', factory.iodbc_registration_documents(profile))
                    part['iodbc_registration'] = True
                    d = child / 'dsn'
                    part['environment'].update({'HOME': str(d), 'ODBCINI': str(d / 'odbc.ini'), 'SYSODBCINI': str(d / 'odbc.ini'), 'ODBCINSTINI': str(d / 'odbcinst.ini'), 'SYSODBCINSTINI': str(d / 'odbcinst.ini')})
                need(len(connection.encode('utf-8')) <= 4096, 'package_private_connection_size')
                part['environment'].update({'ODBCPP_REDSHIFT_PACKAGE_CONNECTION_FILE': connection_file, 'ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER': driver['path'], 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_HOST': config['host'], 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_CA_FILE': local_config['ca'], 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_DATABASE': config['database'], 'ODBCPP_REDSHIFT_PACKAGE_EXPECTED_PRINCIPAL': config['test_user']})
            if private['password_rejection']:
                invalid = factory.build_profile_connection(config, profile, password_rejection=True)
                owned('invalid-connection.txt', invalid)
                part['environment'].update({'ODBCPP_REDSHIFT_AUTH_INVALID_CONNECTION': invalid, 'ODBCPP_REDSHIFT_AUTH_EXPECTED_USER': config['test_user']})
            fd = os.open(child, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        validate_profile_inputs(state, deadline=deadline)
        return state
    except Exception:
        raise ProfileInputBlocked(state) from None

def validate_profile_inputs(state, *, profile_id=None, deadline=None):
    _profile_input_deadline(deadline)
    need(type(state) is dict and state.get('closed') is False, 'profile_inputs_closed')
    directory = Path(state['directory'])
    _profile_directory(directory, state['identity'])
    need({p.name for p in directory.iterdir()} == set(state['profiles']), 'profile_input_untracked')
    keys = list(state['profiles']) if profile_id is None else [profile_id]
    for key in keys:
        _profile_input_deadline(deadline)
        part = state['profiles'][key]
        child = Path(part['directory'])
        _profile_directory(child, part['identity'])
        expected = {Path(x['path']).name for x in part['files']}
        if part['dsn'] is not None:
            expected.add('dsn')
            _validate_private_dsn(part['dsn'])
        need({p.name for p in child.iterdir()} == expected, 'profile_input_untracked')
        for row in part['files']:
            _profile_input_deadline(deadline)
            _profile_held_file(row, deadline=deadline)
        if part['driver'] is not None:
            _profile_held_file(part['driver'], secret=False, deadline=deadline)
        if part['ca'] is not None:
            _profile_held_file(part['ca'], secret=False, deadline=deadline)
        if part.get('iodbc_registration') is True:
            d = child / 'dsn'
            need({p.name for p in d.iterdir()} == {'odbc.ini', 'odbcinst.ini'}, 'iodbc_untracked_registration')
            expected = {'HOME': str(d), 'ODBCINI': str(d / 'odbc.ini'), 'SYSODBCINI': str(d / 'odbc.ini'), 'ODBCINSTINI': str(d / 'odbcinst.ini'), 'SYSODBCINSTINI': str(d / 'odbcinst.ini')}
            need(all((part['environment'].get(k) == v for k, v in expected.items())) and 'ODBCSYSINI' not in part['environment'], 'iodbc_private_selectors_changed')
        need(set(part['environment']) <= PRIVATE_ENV_KEYS, 'private_environment_keys')
    _profile_input_deadline(deadline)

def profile_private_environment(state, key):
    validate_profile_inputs(state, profile_id=key)
    return dict(state['profiles'][key]['environment'])

def close_profile_inputs(state, *, readers_known=True, deadline=None):
    need(readers_known is True, 'private_input_readers_unknown')
    validate_profile_inputs(state, deadline=deadline)
    for part in state['profiles'].values():
        for row in part['files']:
            _profile_input_deadline(deadline)
            _profile_held_file(row, deadline=deadline)
            Path(row['path']).unlink()
            os.close(row['fd'])
            row['body'] = b''
        if part['dsn'] is not None:
            _profile_input_deadline(deadline)
            need(_close_private_dsn(part['dsn']), 'private_dsn_cleanup')
        _profile_directory(Path(part['directory']), part['identity'])
        Path(part['directory']).rmdir()
        if part['driver'] is not None:
            os.close(part['driver']['fd'])
        if part['ca'] is not None:
            os.close(part['ca']['fd'])
        part['environment'].clear()
    _profile_input_deadline(deadline)
    _profile_directory(Path(state['directory']), state['identity'])
    Path(state['directory']).rmdir()
    state['closed'] = True
    return not os.path.lexists(state['directory'])
