"""One admitted native AUTH test window; no admission or executable CLI.

Caller prerequisites (not inferred from flags): canonical lock already held,
independently verified exact admin/control identity and visibility, fresh bound
AWS/TLS/host/named-source controls, and durable finite consumption. Caller selects
protected executable/hash/output paths. Nothing resumes a cluster, discovers an
admin/provider, copies secrets, or grants source/cloud authority here.

Entry fixes execution at +120s and cleanup at +180s. Existing child runner keeps
its reviewed65s bound plus1s reap, so >66s must remain before calling it. Blocking
syscall/process/SQL enforcement still belongs to the caller's outer controller.
"""
import math
from .native_auth_child import CASE, run_selected_child, selected_environment
from .native_auth_cleanup import NativeAuthCleanup


class WindowBlocked(RuntimeError):
    pass


class NativeAuthWindow:
    def __init__(self, *, executable, expected_sha256, selection, directory,
                 sql, clock, runner=run_selected_child):
        if not callable(sql) or not callable(clock) or not callable(runner):
            raise WindowBlocked('invalid_callbacks') from None
        # This validates only the fixed runner's public selection shape, not trust.
        try:
            selected_environment(selection)
        except Exception:
            raise WindowBlocked('invalid_selection') from None
        self._selection = dict(selection)
        self._executable, self._hash, self._directory = executable, expected_sha256, directory
        self._sql, self._clock, self._runner = sql, clock, runner
        self._highwater = None
        self._fault = None
        self._busy = False
        self._used = False

    def _sample(self):
        if self._fault is not None:
            raise WindowBlocked(self._fault) from None
        try:
            now = self._clock()
            valid = type(now) in (int, float) and math.isfinite(now) and now >= 0
        except Exception:
            valid = False
        # The reader may reenter run; first fault cannot be overwritten/healed.
        if self._fault is not None:
            raise WindowBlocked(self._fault) from None
        if not valid:
            self._fault = 'clock_failed'
            raise WindowBlocked(self._fault) from None
        if self._highwater is not None and now < self._highwater:
            self._fault = 'clock_rollback'
            raise WindowBlocked(self._fault) from None
        self._highwater = now
        return now

    def _check(self, cutoff, reason):
        now = self._sample()
        if now >= cutoff:
            raise WindowBlocked(reason) from None
        remaining = cutoff - now
        return math.nextafter(remaining, -math.inf) if type(remaining) is float else remaining

    def run(self):
        if self._busy:
            if self._fault is None:
                self._fault = 'reentrant'
            raise WindowBlocked(self._fault) from None
        if self._used:
            return {'status': 'blocked', 'primary': 'already_consumed',
                    'child': 'not_attempted', 'cleanup': 'not_attempted', 'case': CASE}
        self._used, self._busy = True, True
        primary, child, cleanup = None, 'not_attempted', 'not_attempted'
        attempted = False
        execution_cutoff = cleanup_cutoff = None
        try:
            entered = self._sample()
            execution_cutoff, cleanup_cutoff = entered + 120, entered + 180
            if type(entered) is float:
                # Addition may round upward: use the immediately lower finite
                # representable cutoff, never a later deadline or clock fallback.
                execution_cutoff = math.nextafter(execution_cutoff, -math.inf)
                cleanup_cutoff = math.nextafter(cleanup_cutoff, -math.inf)
            if (not math.isfinite(cleanup_cutoff) or not
                    entered < execution_cutoff < cleanup_cutoff):
                raise WindowBlocked('invalid_cutoffs') from None
            preflight = NativeAuthCleanup(principal=self._selection['EXPECTED_SQL_USER'],
                database=self._selection['DATABASE'], cutoff=execution_cutoff,
                sql=self._sql, clock=self._sample)
            self._check(execution_cutoff, 'execution_deadline')
            try:
                preflight.preflight()
            except Exception:
                raise WindowBlocked(self._fault or 'preflight_failed') from None
            remaining = self._check(execution_cutoff, 'execution_deadline')
            if remaining <= 66:
                raise WindowBlocked('execution_headroom') from None
            attempted = True  # even runner-local refusal requires independent cleanup
            child = 'failed'
            try:
                observed = self._runner(self._executable, self._hash,
                                        dict(self._selection), self._directory)
            except Exception:
                raise WindowBlocked(self._fault or 'child_failed') from None
            self._check(execution_cutoff, 'execution_deadline')
            if (type(observed) is not dict or set(observed) != {'case', 'passed'} or
                    observed['case'] != CASE or type(observed['passed']) is not int or
                    observed['passed'] != 1):
                raise WindowBlocked('child_result_invalid') from None
            child = 'passed'
        except WindowBlocked as error:
            primary = str(error)
        except Exception:
            primary = self._fault or 'window_failed'
        finally:
            if attempted:
                cleanup = 'remote_cleanup_failed'
                try:
                    self._check(cleanup_cutoff, 'cleanup_deadline')
                    remote = NativeAuthCleanup(principal=self._selection['EXPECTED_SQL_USER'],
                        database=self._selection['DATABASE'], cutoff=cleanup_cutoff,
                        sql=self._sql, clock=self._sample)
                    remote.cleanup()
                    self._check(cleanup_cutoff, 'cleanup_deadline')
                    cleanup = 'observed_zero'
                except Exception:
                    cleanup = self._fault or 'remote_cleanup_failed'
            self._busy = False
        return {'status': 'pass' if primary is None and child == 'passed' and
                cleanup == 'observed_zero' else 'blocked',
                'primary': primary, 'child': child, 'cleanup': cleanup, 'case': CASE}
