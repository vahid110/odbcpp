"""Bounded local execution helpers for live tooling.

These helpers grant no live admission. The caller must hold the canonical lock,
persist its reservation, verify AWS controls, and verify server cleanup afterward.
Private raw output is never returned as a diagnostic. Killing the process group
does not prove cancellation of remote SQL.
"""
from dataclasses import dataclass
import os
from pathlib import Path
import signal
import subprocess
import time

from tools.redshift.pilot_preflight import Blocked, validate_private_file


@dataclass(frozen=True)
class ProcessResult:
    returncode: int
    timed_out: bool


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

