"""Synthetic v8 successor policy tests; no cloud, private state or SQL."""
import copy
from decimal import Decimal
import unittest
from datetime import timedelta
from unittest.mock import patch
from tools.redshift import pilot_type_qualification as a
from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_recovery as r
from tools.redshift import test_pilot_finite_amendment as fixture

class TypeQualificationTests(unittest.TestCase):
    def setUp(self):
        self.old = fixture.FiniteAmendmentTests(); self.old.setUp()
        self.addCleanup(self.old.doCleanups)
        self.f, self.c = self.old.f, self.old.c
        with self.old.session() as old:
            self.old.initialize(old)
            self.f.passed(old.reserve(self.c.evidence(), self.c.current))
            self.f.passed(old.transition('active', self.c.cleanup()))
            self.f.passed(old.transition('cleaned_pending_billing', self.c.cleanup()))
        self.c.previous_cleanup = self.c.cleanup()
        self.source = self.f.path.read_text()
        self.previous = self.f.read()[b.OVERLAY]

    def session(self):
        return a.type_qualification_session(self.f.path, self.f.anchor, clock=lambda:self.f.now)

    def review(self):
        result = self.old.review()
        result.update(source_state_digest=r.digest(self.source), inventory=list(a.INVENTORY),
                      metering_ceiling_seconds=13560, cleanup_ceiling_seconds=780)
        return result

    def test_one_type_window_preserves_all_prior_bytes_and_liability(self):
        with self.session() as ledger:
            self.f.passed(ledger.initialize(r.digest(self.source), self.review()))
            self.f.passed(ledger.reserve(self.c.evidence(), self.c.current))
            overlay = self.f.read()[b.OVERLAY]
            self.assertEqual(overlay['schema_version'], 8)
            self.assertEqual(overlay['source_continuation_raw'], self.source)
            self.assertEqual(overlay['attempts'][0]['sequence'], 10)
            self.assertEqual(Decimal(overlay['metering_seconds']), 13560)
            self.assertEqual(Decimal(overlay['cleanup_seconds']), 780)
            self.assertGreaterEqual(Decimal(overlay['total_upper_bound_usd']),
                                    Decimal(self.previous['total_upper_bound_usd']))
            self.f.passed(ledger.transition('active', self.c.cleanup()))
            self.f.passed(ledger.transition('cleaned_pending_billing', self.c.cleanup()))
            before = self.f.path.read_bytes()
            self.f.blocked(ledger.reserve(self.c.evidence(), self.c.current), 'attempt_limit')
            self.assertEqual(before, self.f.path.read_bytes())
        self.assertIsNone(self.f.read()['actual_spend_usd_verified'])
        self.assertIsNone(self.f.read()['remaining_allowance_usd_verified'])
        with self.old.session() as old:
            self.f.blocked(old.reserve(self.c.evidence(), self.c.current))

    def test_changed_source_or_inventory_or_headroom_never_writes(self):
        with self.session() as ledger:
            before = self.f.path.read_bytes()
            self.f.blocked(ledger.initialize('0'*64, self.review()), 'amendment_source_changed')
            for key,value in [('inventory',[]),('max_new_windows',2),
                              ('metering_ceiling_seconds',13561),('cleanup_ceiling_seconds',781),
                              ('original_horizon_only',False),('no_refunds',False)]:
                review = copy.deepcopy(self.review()); review[key] = value
                self.f.blocked(ledger.initialize(r.digest(self.source), review))
                self.assertEqual(before, self.f.path.read_bytes())

    def test_original_period_and_caps_remain_fixed(self):
        from dataclasses import replace
        from datetime import timedelta
        with self.session() as ledger:
            self.f.passed(ledger.initialize(r.digest(self.source), self.review()))
            after = self.f.stamp(self.f.now + timedelta(days=2))
            changed = replace(self.c.current, hard_deadline=after)
            before = self.f.path.read_bytes()
            self.f.blocked(ledger.reserve(self.c.evidence(), changed))
            self.assertEqual(before, self.f.path.read_bytes())

    def test_failed_reservation_persistence_blocks_restart(self):
        with self.session() as ledger:
            self.f.passed(ledger.initialize(r.digest(self.source), self.review()))
            with patch('tools.redshift.pilot_bootstrap.os.replace', side_effect=OSError):
                self.f.blocked(ledger.reserve(self.c.evidence(), self.c.current))
        with self.session() as ledger:
            self.f.blocked(ledger.reserve(self.c.evidence(), self.c.current), 'interrupted_persistence')

    def test_stale_cleanup_and_activation_after_deadline_never_release_headroom(self):
        with self.session() as ledger:
            self.f.passed(ledger.initialize(r.digest(self.source), self.review()))
            self.f.passed(ledger.reserve(self.c.evidence(), self.c.current))
            before = self.f.path.read_bytes()
            cleanup = self.c.cleanup()
            cleanup['observed_at'] = self.f.stamp(self.f.now-timedelta(seconds=301))
            self.f.blocked(ledger.transition('active', cleanup), 'stale_cleanup')
            self.assertEqual(before, self.f.path.read_bytes())
            self.f.now += timedelta(seconds=181)
            self.f.blocked(ledger.transition('active', self.c.cleanup()), 'active_not_admitted')
            self.assertEqual(before, self.f.path.read_bytes())

if __name__ == '__main__': unittest.main()
