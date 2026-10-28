"""Native Windows held-input/HKCU/job/CTest backend extracted from accepted migration004. WSL owns all resource operations; no private historical Python imports."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
"""Private native Windows receiving adapter. Offline unless --authority is given.

WSL alone owns cloud resources, SQL fixtures and principal cleanup. This adapter
accepts test-only inputs on stdin, stages native inputs, configures the existing
CTest project and invokes ONE complete CTest collection. It never parses results.
"""
import argparse
import copy
import ctypes as C
from ctypes import wintypes as W
import datetime as dt
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import sys
import time
import winreg
import subprocess
STARTED = time.monotonic()
LOGGING = dict(LogLevel='Debug', LogFormat='Json', LogSink='Stderr', LogAsync='false', LogQueries='false')

def need(value, code):
    if not value:
        raise RuntimeError(code)

def utc(value):
    parsed = dt.datetime.fromisoformat(value.replace('Z', '+00:00'))
    need(parsed.tzinfo is not None, 'timezone_required')
    return parsed.astimezone(dt.timezone.utc)

def now():
    return dt.datetime.now(dt.timezone.utc)

def json_bytes(value):
    return (json.dumps(value, ensure_ascii=True, indent=2) + '\n').encode('utf-8')

def closed(value, keys, code):
    need(type(value) is dict and set(value) == set(keys), code)

def error_location(error):
    """Public code coordinates only: never messages, source lines or locals."""
    frames, current = ([], error.__traceback__)
    while current is not None and len(frames) < 32:
        code = current.tb_frame.f_code
        frames.append({'file': code.co_filename, 'line': current.tb_lineno, 'function': code.co_name})
        current = current.tb_next
    return frames
K = C.WinDLL('kernel32', use_last_error=True)
A = C.WinDLL('advapi32', use_last_error=True)
P = C.c_void_p

def api(lib, name, args, result=W.BOOL):
    function = getattr(lib, name)
    function.argtypes, function.restype = (args, result)
    return function

def check(value):
    if not value:
        raise C.WinError(C.get_last_error())

class SecurityAttributes(C.Structure):
    _fields_ = [('length', W.DWORD), ('descriptor', P), ('inherit', W.BOOL)]

class FT(C.Structure):
    _fields_ = [('low', W.DWORD), ('high', W.DWORD)]

class Info(C.Structure):
    _fields_ = [('attrs', W.DWORD), ('created', FT), ('accessed', FT), ('written', FT), ('volume', W.DWORD), ('sizeHigh', W.DWORD), ('sizeLow', W.DWORD), ('links', W.DWORD), ('indexHigh', W.DWORD), ('indexLow', W.DWORD)]

class ACL(C.Structure):
    _fields_ = [('revision', C.c_byte), ('unused', C.c_byte), ('size', W.WORD), ('count', W.WORD), ('unused2', W.WORD)]
create_file = api(K, 'CreateFileW', [W.LPCWSTR, W.DWORD, W.DWORD, P, W.DWORD, W.DWORD, W.HANDLE], W.HANDLE)
create_directory = api(K, 'CreateDirectoryW', [W.LPCWSTR, P])
close_handle = api(K, 'CloseHandle', [W.HANDLE])
file_info = api(K, 'GetFileInformationByHandle', [W.HANDLE, C.POINTER(Info)])
file_type = api(K, 'GetFileType', [W.HANDLE], W.DWORD)
read_file = api(K, 'ReadFile', [W.HANDLE, P, W.DWORD, C.POINTER(W.DWORD), P])
write_file = api(K, 'WriteFile', [W.HANDLE, P, W.DWORD, C.POINTER(W.DWORD), P])
flush_file = api(K, 'FlushFileBuffers', [W.HANDLE])
seek_file = api(K, 'SetFilePointerEx', [W.HANDLE, C.c_longlong, P, W.DWORD])
delete_file = api(K, 'DeleteFileW', [W.LPCWSTR])
local_free = api(K, 'LocalFree', [P], P)
get_security = api(A, 'GetSecurityInfo', [W.HANDLE, C.c_int, W.DWORD, C.POINTER(P), P, C.POINTER(P), P, C.POINTER(P)], W.DWORD)
get_ace = api(A, 'GetAce', [P, W.DWORD, C.POINTER(P)])
sid_text = api(A, 'ConvertSidToStringSidW', [P, C.POINTER(W.LPWSTR)])
sddl = api(A, 'ConvertStringSecurityDescriptorToSecurityDescriptorW', [W.LPCWSTR, W.DWORD, C.POINTER(P), P])
open_token = api(A, 'OpenProcessToken', [W.HANDLE, W.DWORD, C.POINTER(W.HANDLE)])
token_info = api(A, 'GetTokenInformation', [W.HANDLE, C.c_int, P, W.DWORD, C.POINTER(W.DWORD)])
current_process = api(K, 'GetCurrentProcess', [], W.HANDLE)

def sid_string(pointer):
    value = W.LPWSTR()
    check(sid_text(pointer, C.byref(value)))
    try:
        return value.value
    finally:
        local_free(C.cast(value, P))

def current_identity():
    token = W.HANDLE()
    check(open_token(current_process(), 8, C.byref(token)))
    try:
        for kind in (1, 4):
            size = W.DWORD()
            token_info(token, kind, None, 0, C.byref(size))
            data = C.create_string_buffer(size.value)
            check(token_info(token, kind, data, size.value, C.byref(size)))
            need(sid_string(C.cast(data, C.POINTER(P))[0]) == SID, 'current_user_and_default_owner_required')
    finally:
        close_handle(token)

class Descriptor:

    def __init__(self, kind='file'):
        rights = 'KA' if kind == 'registry' else 'FA'
        inheritance = 'CI' if kind == 'registry' else 'OICI' if kind == 'directory' else ''
        self.value = P()
        text = 'O:' + SID + 'D:P' + ''.join(('(A;' + inheritance + ';' + rights + ';;;' + who + ')' for who in (SID, 'SY')))
        check(sddl(text, 1, C.byref(self.value), None))
        self.attributes = SecurityAttributes(C.sizeof(SecurityAttributes), self.value, False)

    def close(self):
        if self.value:
            local_free(self.value)
            self.value = P()

def ancestry(path):
    path = Path(path)
    need(path.is_absolute(), 'absolute_native_path')
    for part in (path, *path.parents):
        need(not part.lstat().st_file_attributes & 1024, 'reparse_ancestry')

def restricted_security(handle, object_type=1):
    owner, dacl, descriptor = (P(), P(), P())
    need(get_security(handle, object_type, 5, C.byref(owner), None, C.byref(dacl), None, C.byref(descriptor)) == 0, 'security_read')
    try:
        need(owner and sid_string(owner) == SID and dacl, 'private_owner_dacl')
        entries = C.cast(dacl, C.POINTER(ACL)).contents
        need(entries.count > 0, 'nonempty_private_dacl')
        rights = set()
        for index in range(entries.count):
            ace = P()
            check(get_ace(dacl, index, C.byref(ace)))
            need(C.c_ubyte.from_address(ace.value).value == 0, 'allow_only_dacl')
            who = sid_string(P(ace.value + 8))
            need(who in (SID, 'S-1-5-18'), 'private_dacl_principal')
            if not C.c_ubyte.from_address(ace.value + 1).value & 8:
                mask = W.DWORD.from_address(ace.value + 4).value
                full = 983103 if object_type == 4 else 2032127
                if mask & 268435456 or mask & full == full:
                    rights.add(who)
        need(rights == {SID, 'S-1-5-18'}, 'effective_owner_system_access')
    finally:
        if descriptor:
            local_free(descriptor)

def stamp(value):
    return (value.volume, value.indexHigh, value.indexLow, value.sizeHigh, value.sizeLow, value.attrs, value.links, value.created.low, value.created.high, value.written.low, value.written.high)

class HeldFile:

    def __init__(self, path, private=False):
        self.path, self.private = (Path(path), private)
        self.handle = None
        ancestry(self.path)
        handle = create_file(str(self.path), 2147614720, 1, None, 3, 2097152, None)
        need(handle != C.c_void_p(-1).value, 'held_file_open')
        self.handle = handle
        try:
            value = self.info()
            need(file_type(handle) == 1 and (not value.attrs & 1040) and (value.links == 1) and (value.sizeHigh == 0), 'regular_single_link_file')
            if private:
                restricted_security(handle)
            self.identity = stamp(value)
        except BaseException:
            self.close()
            raise

    def info(self):
        value = Info()
        check(file_info(self.handle, C.byref(value)))
        return value

    def verify(self):
        ancestry(self.path)
        need(stamp(self.info()) == self.identity, 'held_identity_changed')
        if self.private:
            restricted_security(self.handle)

    def read(self, maximum=128 * 1024 * 1024):
        self.verify()
        need(self.identity[4] <= maximum, 'held_file_size_bound')
        check(seek_file(self.handle, 0, None, 0))
        blocks = []
        while True:
            block = C.create_string_buffer(65536)
            count = W.DWORD()
            check(read_file(self.handle, block, len(block), C.byref(count), None))
            if not count.value:
                break
            blocks.append(block.raw[:count.value])
        self.verify()
        return b''.join(blocks)

    def close(self):
        if self.handle:
            close_handle(self.handle)
            self.handle = None

    def retire(self):
        self.verify()
        identity = self.identity
        self.close()
        other = HeldFile(self.path, private=True)
        try:
            need(other.identity == identity, 'retirement_identity_changed')
        finally:
            other.close()
        check(delete_file(str(self.path)))
        need(not self.path.exists(), 'retirement_not_absent')

class PrivateTree:

    def __init__(self):
        self.files, self.directories, self.pending = ([], [], [])

    def mkdir(self, path):
        path = Path(path)
        ancestry(path.parent)
        security = Descriptor('directory')
        try:
            check(create_directory(str(path), C.byref(security.attributes)))
        finally:
            security.close()
        handle = create_file(str(path), 131072, 3, None, 3, 35651584, None)
        need(handle != C.c_void_p(-1).value, 'private_directory_open')
        try:
            restricted_security(handle)
        except BaseException:
            close_handle(handle)
            raise
        self.directories.append((path, handle))
        return path

    def write(self, path, body, retire=True):
        path, body = (Path(path), bytes(body))
        ancestry(path.parent)
        security = Descriptor()
        try:
            handle = create_file(str(path), 3221356544, 0, C.byref(security.attributes), 1, 2097152, None)
        finally:
            security.close()
        need(handle != C.c_void_p(-1).value, 'private_create_new')
        self.pending.append(path)
        try:
            offset = 0
            while offset < len(body):
                written = W.DWORD()
                block = C.create_string_buffer(body[offset:])
                check(write_file(handle, block, len(body) - offset, C.byref(written), None))
                need(written.value > 0, 'private_short_write')
                offset += written.value
            check(flush_file(handle))
            restricted_security(handle)
        finally:
            close_handle(handle)
        held = HeldFile(path, private=True)
        need(held.read(maximum=max(len(body), 1)) == body, 'private_byte_readback')
        if retire:
            self.files.append(held)
        else:
            held.close()
        self.pending.remove(path)
        return path

    def record(self, path, value):
        return self.write(path, json_bytes(value), retire=False)

    def rmdir(self, path):
        path = Path(path)
        row = next((row for row in self.directories if row[0] == path), None)
        need(row is not None, 'owned_directory_required')
        restricted_security(row[1])
        close_handle(row[1])
        self.directories.remove(row)
        path.rmdir()
        need(not path.exists(), 'private_directory_retirement_not_absent')

    def close_handles(self):
        for held in self.files:
            held.close()
        for _, handle in reversed(self.directories):
            close_handle(handle)
        self.directories.clear()

class RegistryDsn:
    """Only exact newly created HKCU keys/value; no existing fixture mutation."""
    ROOT_KEY = 'SOFTWARE\\ODBC'
    PARENT = ROOT_KEY + '\\ODBC.INI'
    SOURCES = PARENT + '\\ODBC Data Sources'
    NAME = 'RedshiftProd'
    VIEW = winreg.KEY_WOW64_64KEY

    def __init__(self):
        self.created, self.expected, self.mapping_created = ([], None, False)
        self.handles = []

    def exists(self, path):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ | self.VIEW):
                return True
        except FileNotFoundError:
            return False

    def create(self, path):
        need(not self.exists(path), 'registry_key_collision')
        security = Descriptor('registry')
        handle, disposition = (W.HANDLE(), W.DWORD())
        create = api(A, 'RegCreateKeyExW', [W.HANDLE, W.LPCWSTR, W.DWORD, W.LPWSTR, W.DWORD, W.DWORD, P, C.POINTER(W.HANDLE), C.POINTER(W.DWORD)], W.LONG)
        reg_close = api(A, 'RegCloseKey', [W.HANDLE], W.LONG)
        try:
            status = create(W.HANDLE(int(winreg.HKEY_CURRENT_USER)), path, 0, None, 0, winreg.KEY_ALL_ACCESS | self.VIEW, C.byref(security.attributes), C.byref(handle), C.byref(disposition))
            need(status == 0 and disposition.value == 1, 'registry_atomic_new_key')
            self.created.append(path)
            restricted_security(handle, 4)
            key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_ALL_ACCESS | self.VIEW)
            self.handles.append(key)
            restricted_security(W.HANDLE(int(key)), 4)
            return key
        finally:
            try:
                if handle.value:
                    need(reg_close(handle) == 0, 'raw_registry_handle_close')
            finally:
                security.close()

    def prepare(self, values):
        need(self.expected is None, 'oneuse_dsn_prepare')
        need(not self.exists(self.PARENT + '\\' + self.NAME), 'dsn_collision')
        if self.exists(self.SOURCES):
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.SOURCES, 0, winreg.KEY_READ | self.VIEW) as key:
                try:
                    winreg.QueryValueEx(key, self.NAME)
                    raise RuntimeError('dsn_mapping_collision')
                except FileNotFoundError:
                    pass
        for path in (self.ROOT_KEY, self.PARENT, self.SOURCES):
            if not self.exists(path):
                self.create(path)
        key = self.create(self.PARENT + '\\' + self.NAME)
        self.expected = {}
        for name, value in values.items():
            winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)
            self.expected[name] = value
        winreg.FlushKey(key)
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.SOURCES, 0, winreg.KEY_SET_VALUE | self.VIEW) as mapping:
            winreg.SetValueEx(mapping, self.NAME, 0, winreg.REG_SZ, DESCRIPTION)
            self.mapping_created = True
            winreg.FlushKey(mapping)
        self.verify()

    def verify(self):
        if self.expected is not None:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.PARENT + '\\' + self.NAME, 0, winreg.KEY_READ | self.VIEW) as key:
                restricted_security(W.HANDLE(int(key)), 4)
                subkeys, count, _ = winreg.QueryInfoKey(key)
                need(subkeys == 0 and count == len(self.expected), 'dsn_shape_changed')
                for name, value in self.expected.items():
                    need(winreg.QueryValueEx(key, name) == (value, winreg.REG_SZ), 'dsn_value_changed')
        if self.mapping_created:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.SOURCES, 0, winreg.KEY_READ | self.VIEW) as key:
                need(winreg.QueryValueEx(key, self.NAME) == (DESCRIPTION, winreg.REG_SZ), 'dsn_mapping_changed')

    def cleanup(self):
        self.verify()
        if self.mapping_created:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.SOURCES, 0, winreg.KEY_SET_VALUE | self.VIEW) as key:
                winreg.DeleteValue(key, self.NAME)
                winreg.FlushKey(key)
            self.mapping_created = False
        for key in self.handles:
            key.Close()
        self.handles.clear()
        for path in reversed(self.created):
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ | self.VIEW) as key:
                restricted_security(W.HANDLE(int(key)), 4)
                subkeys, values, _ = winreg.QueryInfoKey(key)
                if path == self.PARENT + '\\' + self.NAME:
                    need(subkeys == 0 and values == len(self.expected or {}), 'dsn_retire_shape')
                else:
                    need(subkeys == 0 and values == 0, 'owned_registry_parent_not_empty')
            winreg.DeleteKeyEx(winreg.HKEY_CURRENT_USER, path, self.VIEW, 0)
            need(not self.exists(path), 'registry_retirement_not_absent')
        self.created.clear()
        self.expected = None

def native_path(path):
    """A profile path as Windows sees it: WSL /mnt/<drive>/ maps to <DRIVE>:/."""
    need(type(path) is str, 'profile_path')
    mounted = re.fullmatch('/mnt/([a-z])/(.+)', path)
    value = PureWindowsPath(mounted[1].upper() + ':/' + mounted[2] if mounted else path)
    need(value.is_absolute() and '..' not in value.parts, 'absolute_profile_path')
    return value

def factory_module():
    from tools.development import redshift_fixture_factory
    return redshift_fixture_factory

def public_model():
    model = json.loads(MODEL.read_text(encoding='utf-8'))
    need(model['source_head'] == HEAD, 'exact_native_model_source')
    index, expected, specs, profiles = (model[k] for k in ('index', 'expected', 'specs', 'profiles'))
    need(len(index['cases']) == len(expected['cases']) == len(specs) > 0, 'complete_native_inventory')
    rows = []
    for case, wanted, spec in zip(index['cases'], expected['cases'], specs):
        need({k: case[k] for k in ('id', 'image', 'profile', 'gtest_filter')} == wanted and case['id'] == spec['ctest_id'] and (case['gtest_filter'] == spec['case']) and (case['timeout_seconds'] == spec['case_seconds'] == 40), 'ordered_native_inventory')
        image = index['images'][case['image']]
        rows.append((dict(origin_id=case['id'], origin_image=case['image'], host_profile=case['profile'], executable=image['executable'], sha256=image['sha256'], filter=case['gtest_filter']), spec, profiles[spec['profile']]))
    return rows

def validate_payload(payload, rows):
    from tools.development import redshift_powerbi_partition as relationship_partition
    closed(payload, ('schema_version', 'source_head', 'run_id', 'config', 'case_variables'), 'payload_schema')
    need(payload['schema_version'] == 1 and payload['source_head'] == HEAD and (re.fullmatch('windows-live-[A-Za-z0-9_-]{1,60}', payload['run_id']) is not None), 'payload_identity')
    config = payload['config']
    closed(config, ('host', 'port', 'database', 'test_user', 'test_password', 'ca'), 'test_only_config_schema')
    need(config['port'] == 5439 and config['database'] == 'odbcpp_pilot' and (config['test_user'] == 'odbcpp_pilot_test') and (type(config['test_password']) is str) and bool(config['test_password']) and Path(config['ca']).is_absolute(), 'test_config_identity')
    need(type(payload['case_variables']) is list and len(payload['case_variables']) == APPLICABLE_COUNT, 'full_private_case_inventory')
    for entry, (row, spec, profile) in zip(payload['case_variables'], rows):
        closed(entry, ('id', 'variables'), 'case_variable_schema')
        need(entry['id'] == row['origin_id'] and type(entry['variables']) is dict, 'ordered_private_case_binding')
        variables = entry['variables']
        expected = dict(profile['variables'])
        if spec['env'] is not None:
            expected[spec['env']] = spec['marker']
        if spec['case'] == 'RedshiftMetadataWorkflowRealTest.MetadataToPreparedQueryShowConfigured':
            expected.update(ODBCPP_REDSHIFT_TEST_SCHEMA='odbcpp_fixture', ODBCPP_REDSHIFT_TEST_TABLE='pilot_rows')
        dynamic = {name for name in expected if name in ('ODBCPP_REDSHIFT_TIMEOUT_SCHEMA', 'ODBCPP_REDSHIFT_TIMEOUT_TABLE', 'ODBCPP_REDSHIFT_TIMEOUT_OID')}
        if spec['case'] in relationship_partition.CASES:
            observed = {name: variables.get(name) for name in (
                'ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP',
                'ODBCPP_REDSHIFT_POWERBI_PARENT_OID', 'ODBCPP_REDSHIFT_POWERBI_FACT_OID')}
            relationship_partition.profile_variables(profile, observed)
            dynamic.update(('ODBCPP_REDSHIFT_POWERBI_PARENT_OID',
                            'ODBCPP_REDSHIFT_POWERBI_FACT_OID'))
        if spec['case'] == 'RedshiftMetadataScopeShowRealTest.TwoVisibleDatabasesKeepCommonSchemaIdentityThroughWideFanoutAndSibling':
            dynamic.add('ODBCPP_REDSHIFT_METADATA_SCOPE_SECOND_DATABASE')
            expected['ODBCPP_REDSHIFT_METADATA_SCOPE_SECOND_DATABASE'] = ''
        need(set(variables) == set(expected), 'case_variable_inventory')
        for name, value in variables.items():
            need(type(value) is str and len(value) <= 4096 and (not any((c in value for c in '\x00\r\n'))) and ('$<' not in value), 'literal_case_variable')
            if name in dynamic:
                if name.endswith('_OID'):
                    need(re.fullmatch('[1-9][0-9]{0,9}', value) is not None and int(value) <= 4294967295, 'actual_fixture_oid')
                else:
                    need(re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,126}', value) is not None, 'actual_fixture_identifier')
            else:
                need(value == expected[name], 'case_marker_or_oracle_changed')
    return config

def build_inputs(payload, rows, factory, tree, run):
    config = dict(payload['config'], admin_user='odbcpp_pilot_admin', admin_password='')
    directory = tree.mkdir(run / 'private-inputs')
    index = dict(schema_version=1, source_head=HEAD, images={}, profiles={}, cases=[])
    expected = dict(schema_version=1, source_head=HEAD, cases=[])
    dsn_values = None
    package_files = {}
    staged_environments = {}
    for entry, (row, spec, original) in zip(payload['case_variables'], rows):
        profile = copy.deepcopy(original)
        private = profile['private_input']
        # iODBC/unixODBC profiles are POSIX; Windows uses its registered manager.
        manager = profile.get('driver_manager')
        need(manager in (None, 'Windows'), 'posix_driver_manager_profile')
        if manager is not None:
            # The manager loads the registered image, so the profile must
            # already name that exact owned driver, not be rebound to it.
            driver = private['driver']
            need(type(driver) is dict and native_path(driver.get('path')) == PureWindowsPath(DRIVER)
                 and driver.get('sha256') == DRIVER_SHA and driver.get('wchar_bytes') == 2,
                 'manager_profile_owned_driver')
        if private['driver'] is not None:
            private['driver'].update(path=str(DRIVER), sha256=DRIVER_SHA, wchar_bytes=2)
        factory.validate_profile(profile)
        selector = DESCRIPTION if manager is not None else None
        variables = dict(entry['variables'])
        kind = private['kind']
        if kind == 'OWN_DSN':
            text, _ = factory.dsn_documents(config, profile['limits'], str(DRIVER))
            values = dict((line.split('=', 1) for line in text.splitlines()[1:]))
            values.update(DRIVER=str(DRIVER), **LOGGING)
            need(dsn_values is None or dsn_values == values, 'single_native_dsn_profile')
            dsn_values = values
            from tools.development.redshift_powerbi_partition import CASES
            if spec['case'] in CASES:
                variables.update(ODBCPP_TEST_DSN='DSN=' + RegistryDsn.NAME + ';',
                                 ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER=str(DRIVER))
        else:
            if kind == 'OWN_DM_FILE':
                fields = factory.profile_connection_fields(config, profile)
                fields['DRIVER'] = DESCRIPTION
                need(set(fields) == {'DRIVER', 'SERVER', 'PORT', 'DATABASE', 'UID', 'PWD', 'SSL', 'SSLCAFILE'}, 'exact_eight_package_fields')
                connection = ''.join((key + '=' + factory.field(value) + ';' for key, value in fields.items()))
                need(len(connection.encode('utf-8')) <= 4096, 'package_connection_bound')
                key = spec['profile']
                if key not in package_files:
                    package_files[key] = tree.write(directory / (key + '-connection.txt'), connection.encode('utf-8'))
                variables.update(ODBCPP_REDSHIFT_PACKAGE_CONNECTION_FILE=str(package_files[key]), ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER=str(DRIVER), ODBCPP_REDSHIFT_PACKAGE_EXPECTED_HOST=config['host'], ODBCPP_REDSHIFT_PACKAGE_EXPECTED_CA_FILE=config['ca'], ODBCPP_REDSHIFT_PACKAGE_EXPECTED_DATABASE=config['database'], ODBCPP_REDSHIFT_PACKAGE_EXPECTED_PRINCIPAL=config['test_user'])
            else:
                need(kind == 'CORE_ENV', 'own_supported_input_kind')
                connection = factory.build_profile_connection(config, profile, registered_driver=selector)
                connection = connection.rstrip(';') + ';' + ''.join((k + '=' + v + ';' for k, v in LOGGING.items()))
            variables.update({name: connection for name in profile['connection_environment']})
        if private['password_rejection']:
            invalid = factory.build_profile_connection(config, profile, password_rejection=True,
                                                       registered_driver=selector)
            invalid = invalid.rstrip(';') + ';' + ''.join((key + '=' + value + ';' for key, value in LOGGING.items()))
            variables.update(ODBCPP_REDSHIFT_AUTH_INVALID_CONNECTION=invalid, ODBCPP_REDSHIFT_AUTH_EXPECTED_USER=config['test_user'])
        need(all((re.fullmatch('[A-Za-z_][A-Za-z0-9_]*', key) is not None and type(value) is str and (not any((c in value for c in '\x00\r\n'))) and ('$<' not in value) for key, value in variables.items())), 'literal_native_environment')
        selector = row['host_profile']
        environment_body = json_bytes({'environment': [name + '=' + value for name, value in variables.items()]})
        if selector in staged_environments:
            previous_body, environment = staged_environments[selector]
            need(environment_body == previous_body, 'shared_native_profile_environment_changed')
        else:
            environment = tree.write(directory / (selector + '.json'), environment_body)
            staged_environments[selector] = (environment_body, environment)
        index['profiles'][selector] = {'environment_file': str(environment)}
        index['images'][row['origin_image']] = {'executable': row['executable'], 'sha256': row['sha256']}
        case = dict(id=row['origin_id'], image=row['origin_image'], profile=selector, gtest_filter=row['filter'], timeout_seconds=40)
        index['cases'].append(case)
        expected['cases'].append({key: case[key] for key in ('id', 'image', 'profile', 'gtest_filter')})
    need(dsn_values is not None, 'required_native_dsn')
    return (index, expected, dsn_values)

def registration_matches():
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, 'SOFTWARE\\ODBC\\ODBCINST.INI\\' + DESCRIPTION, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
        value, kind = winreg.QueryValueEx(key, 'Driver')
        need(kind == winreg.REG_SZ and Path(value).is_absolute() and (Path(value).resolve(strict=True) == DRIVER.resolve(strict=True)), 'registered_exact_image')
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, 'SOFTWARE\\ODBC\\ODBCINST.INI\\ODBC Drivers', 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as key:
        need(winreg.QueryValueEx(key, DESCRIPTION) == ('Installed', winreg.REG_SZ), 'registered_mapping')

def verify_admission(authority, payload, rows):
    need(authority['schema_version'] == 1 and authority['state'] == 'ROOT_ADMITTED' and (authority['independent_review'] == 'ACCEPT') and (authority['source_head'] == HEAD) and (authority['scope'] == 'complete-applicable-live-native-windows') and (authority['run_id'] == payload['run_id']), 'finite_native_admission')
    need(now() < utc(authority['execute_before_utc']) <= now() + dt.timedelta(minutes=30), 'fresh_native_deadline')
    bound = authority['execution_timeout_seconds']
    need(type(bound) is int and 0 < bound <= 3600, 'finite_native_timeout')
    pins = authority['public_pins']
    required = {str(Path(__file__).resolve()), str(CMAKE), str(CTEST), str(NINJA), str(MODEL), str(DRIVER), str(Path(payload['config']['ca'])), str(REPO / 'tools/development/windows_native_job.py'), str(REPO / 'tools/development/redshift_fixture_factory.py'), str(REPO / 'cmake/LiveSuite.cmake'), str(REPO / 'tests/development/live_suite/CMakeLists.txt')}
    required.add(authority['native']['tools']['git'])
    required.update((row['executable'] for row, _, _ in rows))
    for directory in (*RUNTIME_DIRS, NINJA.parent):
        need(directory.is_dir(), 'native_runtime_directory')
        required.update((str(path) for path in directory.glob('*.dll')))
    need(required <= set(pins), 'all_native_source_tool_image_inputs_pinned')
    need(pins[str(DRIVER)] == DRIVER_SHA, 'driver_pin_binding')
    for row, _, _ in rows:
        need(pins.get(row['executable']) == row['sha256'], 'owning_image_pin')
    held = []
    try:
        for name, digest in pins.items():
            need(type(name) is str and re.fullmatch('[0-9a-f]{64}', digest) is not None and ('provisioned-database-config' not in name) and (Path(name).name.lower() not in ('credentials', 'config', 'connection.txt', 'test.pgpass', 'admin.pgpass')), 'public_pin_only')
            file = HeldFile(name)
            held.append(file)
            need(hashlib.sha256(file.read()).hexdigest() == digest, 'native_public_pin_mismatch')
        git = authority['native']['tools']['git']
        actual = subprocess.check_output([git, '-C', str(REPO), 'rev-parse', 'HEAD'],
                                         text=True, timeout=10).strip()
        dirty = subprocess.check_output([git, '-C', str(REPO), 'status', '--porcelain'],
                                        text=True, timeout=10).strip()
        need(actual == HEAD and not dirty, 'clean_exact_native_source')
        return held
    except BaseException:
        for file in held:
            file.close()
        raise

def native_environment(run):
    system = Path('C:/Windows')
    return {'SystemRoot': str(system), 'WINDIR': str(system), 'COMSPEC': str(system / 'System32/cmd.exe'), 'PATH': ';'.join(map(str, (*RUNTIME_DIRS, system / 'System32', system))), 'TEMP': str(run / 'temp'), 'TMP': str(run / 'temp'), 'LANG': 'C', 'LC_ALL': 'C'}

def configure_command(run):
    """The same exact configure command is used by execution and offline proof."""
    run = Path(run)
    return [str(CMAKE), '-S', str(REPO / 'tests/development/live_suite'), '-B', str(run / 'ctest'), '-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + str(NINJA), '-DODBCPP_LIVE_INDEX=' + str(run / 'index.json'), '-DODBCPP_LIVE_EXPECTED=' + str(run / 'expected.json'), '-DODBCPP_LIVE_OUTPUT=' + str(run / 'gtest')]

def execute(authority, payload):
    current_identity()
    rows = public_model()
    validate_payload(payload, rows)
    held = verify_admission(authority, payload, rows)
    tree, dsn = (PrivateTree(), RegistryDsn())
    run = WORK / payload['run_id']
    lease = None
    run_owned = False
    receipt = dict(source_head=HEAD, applicable_count=APPLICABLE_COUNT, ctest_invocations=0, feature_results_parsed=False, resource_operations=0, SQL_operations=0, readers_known=True, private_inputs_closed=False, dsn_closed=False, jobs=[])
    deadline = STARTED + authority['execution_timeout_seconds']
    code = 72
    try:
        ancestry(WORK)
        need(not run.exists(), 'run_directory_collision')
        lease_path = WORK / 'windows-live-active-lease.json'
        tree.write(lease_path, json_bytes({'run_id': payload['run_id'], 'owner_pid': os.getpid(), 'source_head': HEAD, 'recovery_required_if_interrupted': True}))
        lease = tree.files.pop()
        tree.mkdir(run)
        run_owned = True
        tree.record(run / 'consumption.json', authority)
        for name in ('ctest', 'gtest', 'temp'):
            tree.mkdir(run / name)
        registration_matches()
        factory = factory_module()
        index, expected, values = build_inputs(payload, rows, factory, tree, run)
        dsn.prepare(values)
        tree.record(run / 'index.json', index)
        tree.record(run / 'expected.json', expected)
        environment = native_environment(run)

        def command(argv, label):
            need(time.monotonic() < deadline, 'original_execution_deadline')
            dsn.verify()
            for file in tree.files + held:
                file.verify()
            result = {'phase': label, 'argv': argv, 'treeEmpty': False}
            receipt['jobs'].append(result)
            receipt['readers_known'] = False
            previous = dict(os.environ)
            try:
                os.environ.clear()
                os.environ.update(environment)
                log = run / (label + '-console.log')
                tree.write(log, b'', retire=False)
                with log.open('ab', buffering=0) as output:
                    remaining = deadline - time.monotonic()
                    need(0 < remaining <= 3600, 'native_absolute_execution_cutoff')
                    native = job.run(argv, output, result, timeout=remaining)
            finally:
                os.environ.clear()
                os.environ.update(previous)
                receipt['readers_known'] = all((item.get('treeEmpty') is True and item.get('heldProcessHandlesStopped') is True and (item.get('jobActiveProcesses') == 0) for item in receipt['jobs']))
                tree.record(run / (label + '-job.json'), result)
            need(result.get('treeEmpty') is True and result.get('heldProcessHandlesStopped') is True and (result.get('jobActiveProcesses') == 0), 'native_tree_custody_unknown')
            receipt['readers_known'] = True
            return native
        configure = configure_command(run)
        code = command(configure, 'configure')
        if code == 0:
            receipt['ctest_invocations'] = 1
            command_line = [str(CTEST), '--test-dir', str(run / 'ctest'), '-j1', '--output-on-failure', '--output-junit', str(run / 'ctest-results.xml'), '--output-log', str(run / 'ctest-full.log')]
            code = command(command_line, 'ctest')
        receipt['native_policy_exit'] = code
    except BaseException as error:
        receipt['error_type'] = type(error).__name__
        receipt['error_location'] = error_location(error)
        code = 72
    finally:
        if receipt['readers_known']:
            try:
                dsn.cleanup()
                receipt['dsn_closed'] = True
                if run_owned:
                    candidates = list((run / 'ctest').rglob('CTestTestfile.cmake*'))
                    need(all((path == run / 'ctest/CTestTestfile.cmake' for path in candidates)), 'unexpected_generated_private_metadata_retained')
                    for path in candidates:
                        generated = HeldFile(path, private=True)
                        generated.retire()
                for file in reversed(tree.files):
                    file.retire()
                tree.files.clear()
                need(not tree.pending, 'partial_private_write_retained')
                if run_owned and (run / 'private-inputs').exists():
                    tree.rmdir(run / 'private-inputs')
                receipt['private_inputs_closed'] = True
            except BaseException as error:
                receipt['cleanup_error_type'] = type(error).__name__
                receipt['cleanup_error_location'] = error_location(error)
        safe = receipt['readers_known'] and receipt['private_inputs_closed'] and receipt['dsn_closed']
        receipt['lease_released'] = lease is None
        if safe and lease is not None:
            try:
                lease.retire()
                lease = None
                receipt['lease_released'] = True
            except BaseException as error:
                safe = False
                receipt['lease_cleanup_error_type'] = type(error).__name__
                receipt['lease_cleanup_error_location'] = error_location(error)
        if not safe:
            code = 74
        receipt['exit'] = code
        receipt['native_cleanup_complete'] = safe
        try:
            if run_owned:
                tree.record(run / 'native-receipt.json', receipt)
        finally:
            if lease is not None:
                lease.close()
            tree.close_handles()
            for file in held:
                file.close()
    print(json.dumps({'run_directory': str(run), 'exit': code, 'native_cleanup_complete': receipt['native_cleanup_complete'], 'ctest_invocations': receipt['ctest_invocations'], 'feature_results_parsed': False}))
    return code

def main():
    parser = argparse.ArgumentParser(description='Local Windows CTest/input backend; WSL owns resources.')
    parser.add_argument('--receive', action='store_true')
    parser.add_argument('--initialize-state', type=Path)
    parser.add_argument('--sid', help='Exact current user SID for creating a private state directory')
    args = parser.parse_args()
    if args.initialize_state:
        global SID
        need(args.sid and re.fullmatch(r'S-1-5-21-[0-9-]+', args.sid), 'explicit_current_user_SID')
        SID = args.sid
        current_identity()
        tree = PrivateTree()
        try:
            tree.mkdir(args.initialize_state.resolve())
        finally:
            tree.close_handles()
        print(json.dumps({'private_state_created': str(args.initialize_state.resolve())}))
        return 0
    if not args.receive:
        print('OFFLINE_ONLY: no payload, registry, process, SQL or cloud effects')
        return 0
    raw = sys.stdin.buffer.read(16 * 1024 * 1024 + 1)
    need(len(raw) <= 16 * 1024 * 1024, 'native_payload_bound')
    message = json.loads(raw)
    closed(message, ('authority', 'payload'), 'native_message_schema')
    authority = message['authority']
    configure_context(authority['native'])
    return execute(authority, message['payload'])
from tools.development import windows_native_job as job

def configure_context(native):
    global ROOT, WORK, REPO, HEAD, SID, DRIVER, DRIVER_SHA, DESCRIPTION
    global CMAKE, CTEST, NINJA, MODEL, RUNTIME_DIRS, APPLICABLE_COUNT
    ROOT = Path(native['working_directory']).parent
    WORK = Path(native['working_directory'])
    REPO = Path(native['source_directory'])
    HEAD, SID = (native['source_head'], native['sid'])
    DRIVER, DRIVER_SHA = (Path(native['driver']['path']), native['driver']['sha256'])
    DESCRIPTION = native['driver_description']
    CMAKE, CTEST, NINJA = (Path(native['tools'][key]) for key in ('cmake', 'ctest', 'ninja'))
    MODEL = Path(native['model'])
    RUNTIME_DIRS = tuple((Path(path) for path in native['runtime_directories']))
    APPLICABLE_COUNT = native['case_count']
    need(all((p.is_absolute() for p in (ROOT, WORK, REPO, DRIVER, CMAKE, CTEST, NINJA, MODEL, *RUNTIME_DIRS))) and type(APPLICABLE_COUNT) is int and (APPLICABLE_COUNT > 0) and (re.fullmatch('[0-9a-f]{40}', HEAD) is not None), 'native_configuration')
    need(REPO == Path(__file__).resolve().parents[2], 'fixed_native_source_checkout')
    need(type(DESCRIPTION) is str and factory_module().DRIVER_DESCRIPTION.fullmatch(DESCRIPTION) is not None,
         'registered_driver_description')
if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:
        print(json.dumps({'state': 'WINDOWS_LIVE_STOPPED', 'error_type': type(error).__name__, 'error_location': error_location(error)}))
        raise SystemExit(72)
