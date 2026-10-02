"""Bounded local execution helpers for the Redshift pilot.

These helpers grant no live admission. The caller must hold the canonical lock,
persist its reservation, verify AWS controls, and verify server cleanup afterward.
Private raw output is never returned as a diagnostic. Killing the process group
does not prove cancellation of remote SQL.
"""
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import time
import xml.etree.ElementTree as ET

from tools.redshift.pilot_preflight import Blocked, validate_private_file


@dataclass(frozen=True)
class ProcessResult:
    returncode: int
    timed_out: bool


def verify_executable(path: Path, expected_sha256: str) -> None:
    if not path.is_absolute() or path.is_symlink() or not path.is_file():
        raise Blocked('invalid_executable')
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected_sha256:
        raise Blocked('executable_changed')


def _group_alive(group_id):
    # macOS may return EPERM, rather than ESRCH, for a group containing only
    # reparented zombies. Those cannot execute; independently check membership.
    try:
        result = subprocess.run(['/bin/ps', '-axo', 'pid=,pgid=,stat='],
                                capture_output=True, text=True, timeout=2)
    except (OSError, subprocess.SubprocessError):
        raise Blocked('process_cleanup_unverified') from None
    if result.returncode:
        raise Blocked('process_cleanup_unverified')
    try:
        for line in result.stdout.splitlines():
            pid, pgid, state = line.split()
            int(pid)
            if int(pgid) == group_id and not state.startswith('Z'):
                return True
        return False
    except (ValueError, TypeError):
        raise Blocked('process_cleanup_unverified') from None


def _stop_group(process):
    # Always signal the group, even if its parent already exited.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    except PermissionError:
        if _group_alive(process.pid):
            raise Blocked('process_cleanup_unverified') from None
    try:
        process.wait(timeout=1)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except PermissionError:
        if _group_alive(process.pid):
            raise Blocked('process_cleanup_unverified') from None
    process.wait()
    until = time.monotonic() + 1
    while _group_alive(process.pid):
        if time.monotonic() >= until:
            raise Blocked('process_cleanup_unverified')
        time.sleep(.05)


def bounded_process(argv, *, env, seconds, output: Path, input_bytes=None):
    if type(seconds) is not int or not 1 <= seconds <= 180:
        raise Blocked('invalid_deadline')
    if not argv or not Path(argv[0]).is_absolute():
        raise Blocked('invalid_command')
    # Exclusive creation prevents overwriting or following a credential/log link.
    fd = None
    process = None
    stopped = False
    try:
        fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        validate_private_file(str(output))
        with os.fdopen(fd, 'wb') as log:
            fd = None
            process = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE,
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            timed_out = False
            try:
                process.communicate(input=input_bytes, timeout=seconds)
            except subprocess.TimeoutExpired:
                timed_out = True
            finally:
                stopped = True
                _stop_group(process)
        return ProcessResult(process.returncode, timed_out)
    except (OSError, ValueError):
        raise Blocked('process_unavailable') from None
    finally:
        if process is not None and not stopped:
            _stop_group(process)
        if fd is not None:
            os.close(fd)


def test_summary(xml_path: Path, expected_cases: tuple[str, ...]):
    """Validate exact completed case inventory; omit all assertion/native text."""
    try:
        validate_private_file(str(xml_path))
        if xml_path.stat().st_size > 1024 * 1024:
            raise Blocked('invalid_test_report')
        root = ET.fromstring(xml_path.read_bytes())
        if (root.tag != 'testsuites' or not expected_cases
                or len(expected_cases) != len(set(expected_cases))
                or any(c.tag != 'testsuite' for c in root)
                or root.findall('.//error')):
            raise Blocked('invalid_test_report')
        cases = []
        for suite in root:
            if any(c.tag != 'testcase' for c in suite):
                raise Blocked('invalid_test_report')
            cases.extend(list(suite))
        names = tuple(c.get('classname', '') + '.' + c.get('name', '') for c in cases)
        if len(names) != len(set(names)) or set(names) != set(expected_cases):
            raise Blocked('test_inventory_mismatch')
        if any(c.get('status') != 'run' or c.get('result') != 'completed'
               or c.find('skipped') is not None for c in cases):
            raise Blocked('incomplete_test_run')
        for case in cases:
            if any(c.tag not in ('failure', 'properties') for c in case):
                raise Blocked('invalid_test_report')
            if any(list(c) for c in case.findall('failure')):
                raise Blocked('invalid_test_report')
            for properties in case.findall('properties'):
                if any(p.tag != 'property' or list(p) for p in properties):
                    raise Blocked('invalid_test_report')
        if root.get('errors', '0') != '0':
            raise Blocked('invalid_test_report')
        return {'cases': sorted(names), 'passed': sum(c.find('failure') is None for c in cases),
                'failed': sum(c.find('failure') is not None for c in cases)}
    except (OSError, ET.ParseError, ValueError):
        raise Blocked('invalid_test_report') from None
