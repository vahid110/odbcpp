import hashlib
from pathlib import Path
import unittest
from tools.redshift.provisioned_catalog_fixture import (
    CatalogFixture, FixtureBlocked, SqlReply, CASES, OBJECTS, CREATOR, PROFILES, statements)

FIXTURE = Path(__file__).resolve().parents[2] / 'tests/fixtures/redshift/catalog_contracts.sql'
DIGEST = hashlib.sha256(FIXTURE.read_bytes()).hexdigest()


class Fake:
    def __init__(self):
        self.events, self.calls, self.overrides = [], [], {}
        self.summary = {'cases': list(CASES), 'passed': 5, 'failed': 0}
        self.time = 0

    def sql(self, step, query, seconds):
        self.calls.append((step, query, seconds))
        if step in self.overrides:
            return self.overrides[step]
        if step == 'identity':
            return SqlReply(True, 'odbcpp_pilot|' + CREATOR)
        if step.startswith('collision_'):
            return SqlReply(True, '0')
        if step.startswith('cleanup_'):
            return SqlReply(True, '' if step == 'cleanup_pids' else 'f')
        if step.startswith('owner_'):
            return SqlReply(True, CREATOR + ('|p|2|23|23' if step.endswith(OBJECTS[2]) else '|r'))
        return SqlReply(True)

    def run(self, **kwargs):
        return CatalogFixture(sql=self.sql, tests=lambda cases, seconds: self.summary,
            record=self.events.append, clock=lambda: self.time, deadline=180,
            cleanup_deadline=240, fixture=FIXTURE, fixture_digest=DIGEST, **kwargs).run()


class ProvisionedCatalogFixtureTests(unittest.TestCase):
    def test_modern_primary_key_profile_creates_only_parent_and_runs_fixed_new_inventory(self):
        f = Fake(); objects, cases = PROFILES['modern_primary_key']
        f.summary = {'cases': list(cases), 'passed': 2, 'failed': 0}
        result = f.run(profile='modern_primary_key')
        self.assertEqual('qualified', result['phase'])
        self.assertEqual(list(objects), result['created'])
        self.assertEqual(list(cases), f.events[0]['cases'])
        self.assertEqual(list(objects), f.events[0]['objects'])
        self.assertTrue(result['activity_verified'])
        sql = '\n'.join(q for _, q, _ in f.calls)
        for forbidden in (OBJECTS[1], OBJECTS[2], 'GRANT EXECUTE', 'CALL '):
            self.assertNotIn(forbidden, sql)
        self.assertEqual(1, len([s for s, _, _ in f.calls if s.startswith('grant_')]))

    def test_unknown_profile_and_original_catalog_inventory_cannot_enter_modern_scope(self):
        for profile in ('unknown', '', None, []):
            f = Fake()
            with self.assertRaises(FixtureBlocked):
                f.run(profile=profile)
            self.assertEqual([], f.calls)
            self.assertEqual([], f.events)
        f = Fake()  # Original five-case report cannot stand in for the two new cases.
        result = f.run(profile='modern_primary_key')
        self.assertEqual('blocked', result['phase'])
        self.assertTrue(result['objects_cleaned'])
        self.assertIsNone(result['cases'])

    def test_execution_profile_binds_three_cases_and_parent_only_lifecycle(self):
        f = Fake(); objects, cases = PROFILES['modern_primary_key_execution']
        self.assertEqual(('RedshiftRealTest.ConnectionTest',
            'RedshiftRealTest.ModernPrimaryKeyExecutionContract',
            'RedshiftRealTest.ModernPrimaryKeyExecutionInvalidExactNamesPreserveSession'), cases)
        f.summary = {'cases': list(cases), 'passed': 3, 'failed': 0}
        result = f.run(profile='modern_primary_key_execution')
        self.assertEqual('qualified', result['phase'])
        self.assertEqual(list(objects), f.events[0]['objects'])
        self.assertEqual(list(cases), f.events[0]['cases'])
        self.assertEqual([OBJECTS[0]], result['created'])
        self.assertEqual(['drop_' + OBJECTS[0]], [s for s, _, _ in f.calls if s.startswith('drop_')])
        sql = '\n'.join(q for _, q, _ in f.calls)
        for forbidden in (OBJECTS[1], OBJECTS[2], 'GRANT EXECUTE', 'CALL ', 'CASCADE'):
            self.assertNotIn(forbidden, sql)
        self.assertTrue(result['activity_verified'])

    def test_execution_profile_rejects_old_or_incomplete_case_reports(self):
        cases = PROFILES['modern_primary_key_execution'][1]
        for inventory in (CASES, PROFILES['modern_primary_key'][1], cases[:-1],
                          (cases[0], cases[1], cases[1])):
            f = Fake(); f.summary = {'cases': list(inventory), 'passed': len(inventory), 'failed': 0}
            result = f.run(profile='modern_primary_key_execution')
            self.assertEqual('blocked', result['phase'])
            self.assertIsNone(result['cases'])
            self.assertTrue(result['objects_cleaned'])
            self.assertTrue(result['activity_verified'])

    def test_execution_profile_keeps_edge_failure_and_still_cleans_parent(self):
        f = Fake(); cases = PROFILES['modern_primary_key_execution'][1]
        f.summary = {'cases': list(cases), 'passed': 2, 'failed': 1}
        result = f.run(profile='modern_primary_key_execution')
        self.assertEqual('qualification_failed', result['phase'])
        self.assertEqual(1, result['cases']['failed'])
        self.assertEqual(1, len([e for e in f.events if e['event'] == 'consumed']))
        self.assertTrue(result['objects_cleaned'])
        self.assertTrue(result['activity_verified'])

    def test_success_has_durable_consumption_before_sql_and_selective_reverse_teardown(self):
        f = Fake()
        result = f.run()
        self.assertEqual('qualified', result['phase'])
        self.assertEqual('consumed', f.events[0]['event'])
        self.assertTrue(result['activity_verified'])
        self.assertEqual(list(reversed(OBJECTS)), [s[5:] for s, _, _ in f.calls if s.startswith('drop_')])
        sql = '\n'.join(q for _, q, _ in f.calls)
        self.assertNotIn('CALL ', sql)
        self.assertNotIn('CASCADE', sql)
        self.assertNotIn('CREATE USER', sql)
        self.assertEqual(3, len([s for s, _, _ in f.calls if s.startswith('grant_')]))

    def test_collision_and_wrong_identity_never_create_grant_or_drop(self):
        for step, output in [('identity', 'other|' + CREATOR), ('collision_tables', '1'), ('collision_procedures', '2')]:
            f = Fake(); f.overrides[step] = SqlReply(True, output)
            result = f.run()
            self.assertEqual('cleanup_unverified' if step == 'identity' else 'blocked', result['phase'])
            if step == 'identity':
                self.assertFalse(any(s.startswith('cleanup_') for s, _, _ in f.calls))
            self.assertFalse(any(s.startswith(('create_', 'grant_', 'drop_')) for s, _, _ in f.calls))

    def test_uncertain_create_is_never_replayed_or_dropped(self):
        f = Fake(); f.overrides['create_' + OBJECTS[1]] = SqlReply(False)
        result = f.run()
        self.assertEqual('cleanup_unverified', result['phase'])
        self.assertEqual([OBJECTS[1]], result['uncertain_creations'])
        self.assertFalse(any(s.startswith('drop_') for s, _, _ in f.calls))
        self.assertIsNone(result['cases'])

    def test_test_failures_are_preserved_while_cleanup_succeeds(self):
        f = Fake(); f.summary['passed'] = 2; f.summary['failed'] = 3
        result = f.run()
        self.assertEqual('qualification_failed', result['phase'])
        self.assertTrue(result['objects_cleaned'])
        self.assertEqual(3, result['cases']['failed'])

    def test_incomplete_case_inventory_does_not_qualify(self):
        f = Fake(); f.summary['cases'] = list(CASES[:-1])
        self.assertEqual('blocked', f.run()['phase'])

    def test_unknown_owner_blocks_drop_and_qualification(self):
        f = Fake(); f.overrides['owner_' + OBJECTS[2]] = SqlReply(True, 'someone_else|p|2|23|23')
        result = f.run()
        self.assertEqual('cleanup_unverified', result['phase'])
        self.assertFalse(any(s.startswith('drop_') for s, _, _ in f.calls))

    def test_invalid_pid_never_becomes_sql_interpolation(self):
        f = Fake(); f.overrides['cleanup_pids'] = SqlReply(True, '12);DROP USER x')
        self.assertEqual('cleanup_unverified', f.run()['phase'])
        self.assertFalse(any(s.startswith('terminate_') for s, _, _ in f.calls))

    def test_activity_requires_explicit_false_for_both_checks(self):
        for step in ('cleanup_sessions', 'cleanup_queries'):
            for value in ('', '0', 't'):
                f = Fake(); f.overrides[step] = SqlReply(True, value)
                self.assertEqual('cleanup_unverified', f.run()['phase'])

    def test_fixture_bytes_and_absolute_cleanup_margin_bound_before_consumption(self):
        with self.assertRaises(FixtureBlocked):
            statements(FIXTURE, '0' * 64)
        f = Fake()
        with self.assertRaises(FixtureBlocked):
            CatalogFixture(sql=f.sql, tests=lambda *_: None, record=f.events.append,
                clock=lambda: 0, deadline=180, cleanup_deadline=241,
                fixture=FIXTURE, fixture_digest=DIGEST).run()
        self.assertEqual([], f.events)
        self.assertEqual([], f.calls)

    def test_failed_durable_consumption_prevents_all_sql(self):
        f = Fake()
        def fail(_):
            raise OSError('cannot fsync')
        with self.assertRaises(OSError):
            CatalogFixture(sql=f.sql, tests=lambda *_: None, record=fail,
                clock=lambda: 0, deadline=180, cleanup_deadline=240,
                fixture=FIXTURE, fixture_digest=DIGEST).run()
        self.assertEqual([], f.calls)


class DeadlineAndInventoryTests(unittest.TestCase):
    def test_slow_dispatch_record_never_starts_sql_after_cutoff(self):
        f = Fake()
        def record(event):
            f.events.append(event)
            if event.get('event') == 'dispatch': f.time = 181
        result = CatalogFixture(sql=f.sql, tests=lambda *_: f.summary, record=record,
            clock=lambda: f.time, deadline=180, cleanup_deadline=240,
            fixture=FIXTURE, fixture_digest=DIGEST).run()
        self.assertEqual([], f.calls)
        self.assertEqual('deadline_exhausted', result['reason'])

    def test_duplicate_inventory_cannot_qualify(self):
        f = Fake(); f.summary['cases'].append(CASES[0])
        self.assertEqual('blocked', f.run()['phase'])

    def test_verified_fixture_bytes_are_read_only_once(self):
        data = FIXTURE.read_bytes()
        class Once:
            calls = 0
            def is_symlink(self): return False
            def read_bytes(self):
                self.calls += 1
                if self.calls > 1: raise AssertionError('second read')
                return data
        fixture = Once()
        self.assertEqual(3, len(statements(fixture, DIGEST)))
        self.assertEqual(1, fixture.calls)

    def test_late_create_ack_is_uncertain_and_preserves_all_objects(self):
        f = Fake(); original = f.sql
        def sql(step, query, seconds):
            reply = original(step, query, seconds)
            if step == 'create_' + OBJECTS[1]: f.time = 181
            return reply
        f.sql = sql
        result = f.run()
        self.assertEqual([OBJECTS[1]], result['uncertain_creations'])
        self.assertFalse(any(s.startswith('drop_') for s, _, _ in f.calls))
        self.assertIsNone(result['cases'])

    def test_late_gtest_result_is_not_qualification(self):
        f = Fake()
        def tests(*_):
            f.time = 181
            return f.summary
        result = CatalogFixture(sql=f.sql, tests=tests, record=f.events.append,
            clock=lambda: f.time, deadline=180, cleanup_deadline=240,
            fixture=FIXTURE, fixture_digest=DIGEST).run()
        self.assertEqual('blocked', result['phase'])
        self.assertEqual('late_gtest_result', result['reason'])

    def test_early_cleanup_has_one_sixty_second_cutoff_without_reset(self):
        f = Fake(); original = f.sql
        def sql(step, query, seconds):
            reply = original(step, query, seconds)
            if step == 'cleanup_pids': f.time = 61
            return reply
        f.sql = sql
        result = f.run()
        self.assertEqual('cleanup_unverified', result['phase'])
        self.assertFalse(any(s.startswith('drop_') for s, _, _ in f.calls))
        self.assertEqual(1, sum(s == 'cleanup_pids' for s, _, _ in f.calls))


if __name__ == '__main__':
    unittest.main()
