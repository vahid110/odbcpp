"""Injected exact-principal remote activity checks for one native auth test.

No connection, launcher, credentials, admission or administrator identity proof.
The caller must independently verify its control connection's identity/visibility
and retain the original finite cutoff. A pass describes observed remote activity
only; it is not local secret cleanup, race-free absence or live qualification.
"""
from dataclasses import dataclass
import math
import re

DATABASE = 'odbcpp_pilot'
_PRINCIPAL = re.compile(r'[A-Za-z_][A-Za-z0-9_:.@-]{0,127}', re.ASCII)


@dataclass(frozen=True)
class SqlReply:
    succeeded: bool
    output: str = ''


class CleanupBlocked(RuntimeError):
    """Safe closed reason only; callback/SQL diagnostic text is never included."""


class NativeAuthCleanup:
    def __init__(self, *, principal, database, cutoff, sql, clock):
        if (type(database) is not str or database != DATABASE or
                type(principal) is not str or not _PRINCIPAL.fullmatch(principal) or
                not self._finite(cutoff) or cutoff <= 0 or
                not callable(sql) or not callable(clock)):
            raise CleanupBlocked('invalid_selection')
        self._database, self._principal, self._cutoff = database, principal, cutoff
        self._sql, self._clock = sql, clock
        self._highwater = None
        self._fault = None
        self._busy = False
        self._preflight_used = False
        self._cleanup_used = False

    @staticmethod
    def _finite(value):
        if type(value) not in (int, float):
            return False
        try:
            return math.isfinite(value)
        except (OverflowError, ValueError):
            return False

    def _refuse(self, reason):
        if self._fault is None:
            self._fault = reason
        raise CleanupBlocked(self._fault) from None

    def _sample(self):
        if self._fault is not None:
            raise CleanupBlocked(self._fault) from None
        try:
            now = self._clock()
        except Exception:
            self._refuse('clock_failed')
        # A clock callback can reenter the helper; it cannot heal that fault.
        if self._fault is not None:
            raise CleanupBlocked(self._fault) from None
        if not self._finite(now) or now < 0:
            self._refuse('clock_failed')
        if self._highwater is not None and now < self._highwater:
            self._refuse('clock_rollback')
        self._highwater = now
        if now >= self._cutoff:
            self._refuse('deadline_exhausted')
        remaining = self._cutoff - now
        if not self._finite(remaining) or remaining <= 0:
            self._refuse('clock_failed')
        return remaining

    def _dispatch(self, step, query):
        remaining = self._sample()
        try:
            reply = self._sql(step, query, min(15, remaining))
        except Exception:
            self._refuse('sql_failed')
        self._sample()  # equality is expired; no late acknowledgment accepted
        if (type(reply) is not SqlReply or reply.succeeded is not True or
                type(reply.output) is not str or len(reply.output) > 128):
            self._refuse('bad_reply')
        if reply.output == '\n':
            self._refuse('bad_reply')
        # One psql record terminator is allowed, never arbitrary truthy/whitespace.
        return reply.output.removesuffix('\n')

    def _predicate(self, pid_column):
        # The exact validated selection is never expanded to another IAM form.
        return ("user_name='" + self._principal + "' AND db_name='" + self._database +
                "' AND " + pid_column + ' <> pg_backend_pid()')

    def _zero_activity(self, prefix):
        sessions = ('SELECT EXISTS(SELECT 1 FROM stv_sessions WHERE ' +
                    self._predicate('process') + ');')
        queries = ('SELECT EXISTS(SELECT 1 FROM stv_recents WHERE ' +
                   self._predicate('pid') + " AND status <> 'Done');")
        # Independently observe both families. Never terminate in preflight.
        if self._dispatch(prefix + '_sessions', sessions) != 'f':
            self._refuse('sessions_not_zero')
        if self._dispatch(prefix + '_queries', queries) != 'f':
            self._refuse('queries_not_zero')

    def _enter(self, cleanup):
        if self._fault is not None:
            raise CleanupBlocked(self._fault) from None
        if self._busy:
            self._refuse('reentrant')
        if self._cleanup_used or (not cleanup and self._preflight_used):
            self._refuse('already_consumed')
        self._busy = True
        if cleanup:
            self._cleanup_used = True
        else:
            self._preflight_used = True

    def preflight(self):
        """Refuse preexisting exact-principal activity without terminating it."""
        self._enter(False)
        try:
            self._zero_activity('preflight')
            return {'status': 'pass', 'remote_activity': 'observed_zero'}
        finally:
            self._busy = False

    def cleanup(self):
        """One bounded termination attempt followed by separate zero checks."""
        self._enter(True)
        try:
            output = self._dispatch('cleanup_pids', 'SELECT process FROM stv_sessions WHERE ' +
                                    self._predicate('process') + ';')
            pids = output.split('\n') if output else []
            if (len(pids) > 8 or len(pids) != len(set(pids)) or any(
                    not re.fullmatch(r'[1-9][0-9]{0,9}', pid, re.ASCII) or
                    int(pid) > 2147483647 for pid in pids)):
                self._refuse('invalid_pid_inventory')
            for pid in pids:
                if self._dispatch('terminate_' + pid, 'SELECT pg_terminate_backend(' + pid + ');') != 't':
                    self._refuse('termination_unverified')
            self._zero_activity('cleanup')
            return {'status': 'pass', 'remote_activity': 'observed_zero', 'terminated': len(pids)}
        finally:
            self._busy = False
