"""One cleanup-only successor for unresolved010; no qualification or cloud calls."""
from contextlib import contextmanager
from datetime import datetime, timezone
from decimal import Decimal, ROUND_CEILING
from pathlib import Path
from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_recovery as recovery
from tools.redshift import pilot_type_qualification as prior


class TypeRecoverySession(recovery.RecoverySession):
    TARGET_SEQUENCE = 10
    SCHEMA_VERSION = 9
    SOURCE_ATTEMPTS = 1

    def _prior_overlay(self, original, now):
        validator = prior.TypeQualificationSession(self.fd, self.directory, self.anchor, self.clock)
        try:
            return validator._v2(original, now)
        finally:
            validator.closed = True

    def _amounts(self, deadline, metering, cleanup, floor_compute, floor_total, now):
        duration = b._seconds(b._time(deadline, now, True)-b._time(self.anchor.earliest_billable_start, now))
        r0._need(0 < duration <= 86400 and metering == 14760 and cleanup == 840,
                 'invalid_type_recovery_bound')
        compute = max(floor_compute, (Decimal(4)*Decimal('.374')*(duration+metering+cleanup)/3600)
                      .quantize(Decimal('.01'), rounding=ROUND_CEILING))
        total = max(floor_total, compute+50)
        r0._need(compute <= 100 and total <= 150, 'budget_exhausted')
        return compute, total


@contextmanager
def type_recovery_session(path, original_anchor, *, clock=None):
    path = Path(path); r0._need(path.name == 'setup.json', 'invalid_state_path')
    with r0.pilot_lock(path.parent) as fd:
        session = TypeRecoverySession(fd, path.parent, original_anchor,
            clock or (lambda: datetime.now(timezone.utc)))
        try:
            yield session
        finally:
            session.closed = True
