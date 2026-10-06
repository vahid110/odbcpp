"""Fixed native AUTH child execution; caller supplies separately reviewed admission.

No cloud calls, cluster resume, credentials creation or admission CLI. The caller
must durably consume its finite scope before this function, and independently
verify exact-principal remote cleanup afterward even if this function fails.
"""
import hashlib
import json
import os
from pathlib import Path
import selectors
import subprocess
import time
import xml.etree.ElementTree as ET

CASE = 'ProvisionedNativeAuth.GetClusterCredentialsThenVerifiedTlsLogin'
MARKER = 'provisioned-native-acquisition-login001'
KEYS = frozenset('ACCOUNT REGION CLUSTER DATABASE REQUESTED_DB_USER EXPECTED_SQL_USER DB_HOST DB_CA_FILE PROFILE CREDENTIALS_FILE CONFIG_FILE SOURCE_IDENTITY SOURCE_GENERATION'.split())
PREFIX = 'ODBCPP_REDSHIFT_NATIVE_'
OUTPUT_CAP = 1024 * 1024


def selected_environment(selection):
    if type(selection) is not dict or set(selection) != KEYS:
        raise ValueError('selection_inventory')
    for key, value in selection.items():
        limit = 4096 if key.endswith('_FILE') else 256
        if (type(value) is not str or not 1 <= len(value) <= limit
                or any(ord(c) < 32 or ord(c) == 127 for c in value)):
            raise ValueError('selection_value')
    for key in ('DB_CA_FILE', 'CREDENTIALS_FILE', 'CONFIG_FILE'):
        if not selection[key].startswith('/'):
            raise ValueError('selection_path')
    # Intentionally no HOME, AWS profile/config, proxy, role, web identity,
    # credential-process, endpoint override or ambient fixture variables.
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C',
           'AWS_EC2_METADATA_DISABLED': 'true',
           'ODBCPP_REDSHIFT_NATIVE_AUTH_ADMISSION': MARKER}
    env.update({PREFIX + key: value for key, value in selection.items()})
    return env


def completed_case(data):
    if type(data) is not bytes or not 0 < len(data) <= OUTPUT_CAP:
        raise ValueError('report_size')
    if b'<!' in data or b'<?' in data.replace(b'<?xml version="1.0" encoding="UTF-8"?>', b'', 1):
        raise ValueError('report_declaration')
    root = ET.fromstring(data)
    suites = list(root)
    if root.tag != 'testsuites' or len(suites) != 1 or suites[0].tag != 'testsuite':
        raise ValueError('report_inventory')
    cases = list(suites[0])
    if len(cases) != 1 or cases[0].tag != 'testcase':
        raise ValueError('report_inventory')
    case = cases[0]
    if (case.get('classname', '') + '.' + case.get('name', '') != CASE
            or case.get('status') != 'run' or case.get('result') != 'completed'
            or root.get('tests') != '1' or suites[0].get('tests') != '1'
            or any(node.get(k, '0') != '0' for node in (root, suites[0])
                   for k in ('failures', 'errors', 'disabled'))
            or list(case)):
        raise ValueError('report_incomplete')
    return {'case': CASE, 'passed': 1}


def private_record(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w') as output:
        json.dump(value, output, sort_keys=True)
        output.write('\n')
        output.flush()
        os.fsync(output.fileno())


def cleanup_child(process, reason, returncode):
    """Observe the direct child despite a poll/kill exit race; never heal failure."""
    failed = False
    try:
        running = process.poll() is None
    except Exception:
        running = True
        failed = True
    if running:
        try:
            process.kill()
        except ProcessLookupError:
            # Still require an independently observed wait below.
            pass
        except Exception:
            failed = True
    try:
        observed = process.wait(timeout=1)
        if type(observed) is not int:
            raise ValueError('unknown_child_status')
        returncode = observed
    except Exception:
        failed = True
        returncode = None
    try:
        if process.stdout is not None:
            process.stdout.close()
    except Exception:
        failed = True
    if failed:
        reason = 'local_cleanup_unverified'
    return reason, returncode


def run_selected_child(executable, expected_sha256, selection, directory):
    """Only invoked by the future admitted controller; never grants admission.

    Selected SDK scope may create threads but must not exec/spawn children.
    Direct-child cleanup is not a remote session cancellation guarantee.
    """
    started = time.monotonic()
    deadline = started + 65  # GTest has its own single 60-second entry deadline.
    executable, directory = Path(executable), Path(directory)
    env = selected_environment(selection)
    if (not executable.is_absolute() or executable.is_symlink()
            or not executable.is_file()
            or hashlib.sha256(executable.read_bytes()).hexdigest() != expected_sha256):
        raise ValueError('executable_changed')
    stat = directory.lstat()
    if (not directory.is_absolute() or directory.is_symlink()
            or not directory.is_dir() or stat.st_uid != os.geteuid()
            or stat.st_mode & 0o777 != 0o700):
        raise ValueError('private_directory')
    xml_path = directory / 'tests.xml'
    xml_fd = os.open(xml_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    os.close(xml_fd)
    log_fd = os.open(directory / 'output.log', os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    process = None
    reason, returncode = 'launch_failure', None
    count = 0
    try:
        with os.fdopen(log_fd, 'wb') as log, selectors.DefaultSelector() as selector:
            if time.monotonic() >= deadline:
                raise ValueError('prelaunch_deadline')
            process = subprocess.Popen([str(executable), '--gtest_filter=' + CASE,
                '--gtest_repeat=1', '--gtest_output=xml:' + str(xml_path)],
                env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, close_fds=True, start_new_session=True)
            os.set_blocking(process.stdout.fileno(), False)
            selector.register(process.stdout, selectors.EVENT_READ)
            reason = 'completed'
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    reason = 'deadline'; break
                for key, _ in selector.select(min(remaining, 0.1)):
                    chunk = os.read(key.fd, min(65536, OUTPUT_CAP - count + 1))
                    if not chunk:
                        selector.unregister(key.fileobj); continue
                    if len(chunk) > OUTPUT_CAP - count:
                        reason = 'output_limit'; break
                    log.write(chunk); count += len(chunk)
                if reason != 'completed':
                    break
            if reason == 'completed':
                try:
                    returncode = process.wait(timeout=max(0, deadline - time.monotonic()))
                    if time.monotonic() >= deadline:
                        reason = 'deadline'
                except subprocess.TimeoutExpired:
                    reason = 'deadline'
    except Exception:
        reason = 'execution_failure' if process is not None else 'launch_failure'
    finally:
        if process is not None:
            reason, returncode = cleanup_child(process, reason, returncode)
        if reason == 'completed' and time.monotonic() >= deadline:
            reason = 'deadline'
        # Durable observed status BEFORE XML parsing. Unknown exit is never zero.
        private_record(directory / 'child-status.json', {
            'returncode': returncode, 'reason': reason,
            'output_bytes': count, 'remote_cleanup_verified': False})
    if reason != 'completed' or type(returncode) is not int or returncode != 0:
        raise ValueError('native_child_failed')
    with xml_path.open('rb') as report:
        summary = completed_case(report.read(OUTPUT_CAP + 1))
    private_record(directory / 'local-result.json', {
        **summary, 'returncode': returncode, 'remote_cleanup_verified': False})
    return summary
