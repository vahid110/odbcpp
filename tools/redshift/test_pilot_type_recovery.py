"""Offline010 cleanup accounting tests; no cloud/SQL."""
from dataclasses import replace
from datetime import timedelta
from types import SimpleNamespace
import json
from tools.redshift import pilot_type_recovery as ledger
from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_recovery as r
from tools.redshift import test_pilot_type_qualification as types
from tools.redshift import test_pilot_recovery as old


class TypeRecoveryTests(old.RecoveryTests):
    def setUp(self):
        self.types = types.TypeQualificationTests(); self.types.setUp()
        self.addCleanup(self.types.doCleanups); self.f = self.types.f
        with self.types.session() as s:
            self.f.passed(s.initialize(r.digest(self.types.source), self.types.review()))
            self.f.passed(s.reserve(self.types.c.evidence(), self.types.c.current))
            self.f.passed(s.transition('uncertain'))
        self.source = self.f.path.read_text()
        self.report = json.dumps(dict(status='blocked', reason='remote_cleanup_unverified',
            cleanup_verified=False, results=[], actual_spend_usd=None,
            remaining_allowance_usd=None, sequence=10), indent=2)+'\n'
        self.result = self.f.path.parent/'window-010-result.json'
        self.result.write_text(self.report); self.result.chmod(0o600)
        self.f.now += timedelta(minutes=6)
        self.current = replace(self.f.anchor, hard_deadline=self.f.stamp(self.f.now+timedelta(seconds=60)))
        self.windows = SimpleNamespace(session=self.types.session,
            evidence=self.types.c.evidence, current=self.current)

    def session(self):
        return ledger.type_recovery_session(self.f.path, self.f.anchor, clock=lambda:self.f.now)

    def test_original_horizon_and_exact_scope_are_fixed(self):
        before = self.f.path.read_bytes()
        current = replace(self.current, hard_deadline=self.f.stamp(self.f.now+timedelta(days=1)))
        with self.session() as s:
            self.f.blocked(s.reserve_recovery(self.evidence(), current))
        self.assertEqual(before, self.f.path.read_bytes())

    def test_retained_floors_cannot_bypass_budget_caps(self):
        from decimal import Decimal
        from tools.redshift.pilot_preflight import Blocked
        with self.session() as s:
            with self.assertRaisesRegex(Blocked, 'budget_exhausted'):
                s._amounts(self.current.hard_deadline, Decimal(14760), Decimal(840),
                           Decimal(101), Decimal(151), self.f.now)
