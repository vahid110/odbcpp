import hashlib
from pathlib import Path
import unittest
from tools.redshift.provisioned_catalog_fixture import (
    CatalogFixture, FixtureBlocked, SqlReply, CASES, OBJECTS, CREATOR, PROFILES, statements, EDGE_OBJECTS, QUOTED_KEY, edge_statements)

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
        if step == 'schema_usage':
            return SqlReply(True, 'schema_usage_allowed')
        if step.startswith('absence_'):
            return SqlReply(True, '0')
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
    def missing_run(self, f, tests=None):
        return CatalogFixture(sql=f.sql, tests=tests or (lambda cases, seconds: f.summary),
            record=f.events.append, clock=lambda: f.time, deadline=180,
            cleanup_deadline=240, fixture=FIXTURE, fixture_digest=DIGEST,
            profile='primary_key_missing_legacy').run()

    def missing_fake(self):
        f = Fake()
        cases = PROFILES['primary_key_missing_legacy'][1]
        f.summary = {'cases': list(cases), 'passed': 2, 'failed': 0}
        return f

    def test_missing_inventory_and_absence_checks_never_mutate_objects(self):
        f = self.missing_fake(); result = self.missing_run(f)
        self.assertEqual('qualified', result['phase'])
        self.assertTrue(result['absence_verified']); self.assertTrue(result['activity_verified'])
        self.assertEqual([], result['created']); self.assertEqual([], f.events[0]['objects'])
        self.assertEqual(2, len(f.events[0]['cases']))
        steps = [step for step, _, _ in f.calls]
        self.assertLess(steps.index('schema_usage'), steps.index('absence_before'))
        self.assertLess(steps.index('cleanup_queries'), steps.index('absence_before'))
        self.assertLess(steps.index('absence_before'), steps.index('absence_after'))
        self.assertFalse(any(word in query for _, query, _ in f.calls
            for word in ('CREATE ', 'GRANT ', 'DROP ', 'CALL ', "IN ('')")))

    def test_missing_pre_absence_refusals_do_not_run_cases(self):
        for reply in (SqlReply(True, '1'), SqlReply(True, ''), SqlReply(True, 'NULL'),
                SqlReply(True, '0\n0'), SqlReply(False, '0')):
            f = self.missing_fake(); f.overrides['absence_before'] = reply
            result = self.missing_run(f, lambda *_: self.fail('cases dispatched'))
            self.assertEqual('blocked', result['phase'])
            self.assertIsNone(result['cases']); self.assertTrue(result['activity_verified'])
            self.assertFalse(result['absence_verified']); self.assertFalse(result['absence_before_verified'])
            self.assertTrue(result['absence_after_verified'])

    def test_missing_post_absence_failure_still_checks_activity_and_never_drops(self):
        for reply in (SqlReply(True, '1'), SqlReply(False), SqlReply(True, '')):
            f = self.missing_fake(); f.overrides['absence_after'] = reply
            result = self.missing_run(f)
            self.assertEqual('cleanup_unverified', result['phase'])
            self.assertEqual('qualified', result['qualification_phase'])
            self.assertFalse(result['absence_verified']); self.assertTrue(result['activity_verified'])
            steps = [step for step, _, _ in f.calls]
            self.assertIn('cleanup_queries', steps[steps.index('absence_after') + 1:])
            self.assertFalse(any(step.startswith('drop_') for step in steps))

    def test_missing_inventory_and_callback_failures_still_check_post_absence(self):
        for kind in ('inventory', 'callback', 'late'):
            f = self.missing_fake()
            def tests(*_):
                if kind == 'callback': raise RuntimeError('private')
                if kind == 'late': f.time = 181
                return {'cases': list(CASES), 'passed': 5, 'failed': 0}
            result = self.missing_run(f, tests)
            self.assertEqual('blocked', result['phase'])
            self.assertTrue(result['absence_verified']); self.assertTrue(result['activity_verified'])
            self.assertIn('absence_after', [step for step, _, _ in f.calls])

    def test_missing_pre_activity_cannot_use_cleanup_reservation(self):
        f = self.missing_fake(); original = f.sql
        def sql(step, query, seconds):
            reply = original(step, query, seconds)
            if step == 'cleanup_pids' and f.time == 0: f.time = 181
            return reply
        f.sql = sql
        result = self.missing_run(f, lambda *_: self.fail('cases dispatched'))
        self.assertEqual('late_sql_acknowledgment', result['reason'])
        self.assertEqual('blocked', result['phase'])
        self.assertFalse(result['absence_verified']); self.assertTrue(result['absence_after_verified'])
        self.assertTrue(result['activity_verified'])
        self.assertNotIn('absence_before', [step for step, _, _ in f.calls])

    def test_modes_inventory_parent_only_and_failed_cases_keep_cleanup_evidence(self):
        f = Fake(); objects, cases = PROFILES['primary_key_modes']
        self.assertEqual((OBJECTS[0],), objects)
        self.assertEqual(6, len(cases)); self.assertEqual(6, len(set(cases)))
        f.summary = {'cases': list(cases), 'passed': 4, 'failed': 2}
        result = f.run(profile='primary_key_modes')
        self.assertEqual('qualification_failed', result['phase'])
        self.assertTrue(result['objects_cleaned']); self.assertTrue(result['activity_verified'])
        self.assertEqual(list(cases), f.events[0]['cases'])
        self.assertEqual([OBJECTS[0]], result['created'])
        sql = '\n'.join(q for _, q, _ in f.calls)
        for forbidden in (OBJECTS[1], OBJECTS[2], 'GRANT EXECUTE', 'CALL ', 'CASCADE'):
            self.assertNotIn(forbidden, sql)
        self.assertEqual(1, len([s for s, _, _ in f.calls if s.startswith('grant_')]))
        self.assertEqual(['drop_' + OBJECTS[0]], [s for s, _, _ in f.calls if s.startswith('drop_')])

    def test_modes_inventory_rejects_prior_missing_duplicate_and_substituted_reports(self):
        cases = PROFILES['primary_key_modes'][1]
        for inventory in (CASES, PROFILES['modern_primary_key_execution'][1],
                cases[:-1], cases[:-1] + (cases[0],), cases[:-1] + ('RedshiftRealTest.VersionQuery',)):
            f = Fake(); f.summary = {'cases': list(inventory), 'passed': len(inventory), 'failed': 0}
            result = f.run(profile='primary_key_modes')
            self.assertEqual('blocked', result['phase'])
            self.assertTrue(result['objects_cleaned']); self.assertTrue(result['activity_verified'])
            self.assertIsNone(result['cases'])

    def test_edge_profile_escapes_exact_names_and_preserves_inventory(self):
        f = Fake(); objects, cases = PROFILES['primary_key_edges']
        fixture = FIXTURE.with_name('primary_key_edges.sql')
        digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
        f.summary = {'cases': list(cases), 'passed': 5, 'failed': 0}
        result = CatalogFixture(sql=f.sql, tests=lambda cases, seconds: f.summary, record=f.events.append, clock=lambda: f.time,
            deadline=180, cleanup_deadline=240, fixture=fixture, fixture_digest=digest,
            profile='primary_key_edges').run()
        self.assertEqual('qualified', result['phase'])
        self.assertEqual(list(objects), result['created'])
        self.assertTrue(result['objects_cleaned']); self.assertTrue(result['activity_verified'])
        self.assertEqual(list(cases), f.events[0]['cases'])
        queries = dict((step, query) for step, query, _ in f.calls)
        self.assertIn("a\"b%''_", queries['collision_tables'])
        self.assertIn("a\"b%''_", queries['owner_' + QUOTED_KEY])
        self.assertIn('a""b%', queries['grant_' + QUOTED_KEY])
        self.assertIn('a""b%', queries['drop_' + QUOTED_KEY])
        self.assertTrue(queries['drop_' + QUOTED_KEY].endswith(' RESTRICT;'))
        self.assertFalse(any('CASCADE' in q or 'EXECUTE ON' in q or 'CALL ' in q for q in queries.values()))
        self.assertEqual(2, len(edge_statements(fixture, digest)))

    def test_edge_usage_refusal_occurs_before_creation(self):
        f = Fake(); f.overrides['schema_usage'] = SqlReply(True, 'false')
        fixture = FIXTURE.with_name('primary_key_edges.sql')
        result = CatalogFixture(sql=f.sql, tests=lambda cases, seconds: f.summary, record=f.events.append, clock=lambda: f.time,
            deadline=180, cleanup_deadline=240, fixture=fixture,
            fixture_digest=hashlib.sha256(fixture.read_bytes()).hexdigest(),
            profile='primary_key_edges').run()
        self.assertEqual('schema_usage_unverified', result['reason'])
        self.assertEqual([], result['created'])
        self.assertFalse(any(step.startswith('create_') for step, _, _ in f.calls))
        self.assertTrue(result['objects_cleaned']); self.assertTrue(result['activity_verified'])

    def edge_run(self, f):
        fixture = FIXTURE.with_name('primary_key_edges.sql')
        return CatalogFixture(sql=f.sql, tests=lambda cases, seconds: f.summary,
            record=f.events.append, clock=lambda: f.time, deadline=180,
            cleanup_deadline=240, fixture=fixture,
            fixture_digest=hashlib.sha256(fixture.read_bytes()).hexdigest(),
            profile='primary_key_edges').run()

    def test_edge_uncertain_or_late_second_create_preserves_all_objects(self):
        for late in (False, True):
            f = Fake(); original = f.sql
            def sql(step, query, seconds):
                if step == 'create_' + QUOTED_KEY:
                    if late:
                        f.time = 181
                        return SqlReply(True)
                    return SqlReply(False)
                return original(step, query, seconds)
            f.sql = sql
            result = self.edge_run(f)
            self.assertEqual([EDGE_OBJECTS[0]], result['created'])
            self.assertEqual([QUOTED_KEY], result['uncertain_creations'])
            self.assertEqual('cleanup_unverified', result['phase'])
            self.assertFalse(any(step.startswith('drop_') for step, _, _ in f.calls))
            self.assertIsNone(result['cases'])

    def test_edge_second_grant_failure_cleans_confirmed_creations(self):
        f = Fake(); f.overrides['grant_' + QUOTED_KEY] = SqlReply(False)
        result = self.edge_run(f)
        self.assertEqual('sql_step_failed', result['reason'])
        self.assertEqual(list(EDGE_OBJECTS), result['created'])
        self.assertTrue(result['objects_cleaned']); self.assertTrue(result['activity_verified'])
        self.assertEqual(['drop_' + n for n in reversed(EDGE_OBJECTS)],
            [step for step, _, _ in f.calls if step.startswith('drop_')])
        self.assertIsNone(result['cases'])

    def test_edge_rejects_old_incomplete_duplicate_or_substituted_inventory(self):
        cases = list(PROFILES['primary_key_edges'][1])
        for inventory in (list(CASES), cases[:-1], cases[:-1] + [cases[0]],
                          cases[:-1] + ['RedshiftRealTest.ConnectionTestSubstitute']):
            f = Fake(); f.summary = {'cases': inventory, 'passed': len(inventory), 'failed': 0}
            result = self.edge_run(f)
            self.assertEqual('invalid_gtest_inventory', result['reason'])
            self.assertTrue(result['objects_cleaned']); self.assertTrue(result['activity_verified'])

    def test_edge_quoted_owner_mismatch_blocks_drop(self):
        f = Fake(); f.summary = {'cases': list(PROFILES['primary_key_edges'][1]), 'passed': 5, 'failed': 0}
        f.overrides['owner_' + QUOTED_KEY] = SqlReply(True, 'other_owner|r')
        result = self.edge_run(f)
        self.assertEqual('cleanup_unverified', result['phase'])
        self.assertFalse(any(step.startswith('drop_') for step, _, _ in f.calls))
        self.assertFalse(result['objects_cleaned'])

    def test_edge_usage_exact_canonical_marker_and_failures(self):
        for reply in (SqlReply(True, 't'), SqlReply(True, 'true'), SqlReply(True, 'schema_usage_blocked'),
                      SqlReply(True, ''), SqlReply(True, 'NULL'),
                      SqlReply(True, 'schema_usage_allowed\nschema_usage_allowed'), SqlReply(False)):
            f = Fake(); f.overrides['schema_usage'] = reply
            result = self.edge_run(f)
            self.assertIn(result['reason'], ('schema_usage_unverified', 'sql_step_failed'))
            self.assertFalse(result['created'])
            self.assertFalse(any(step.startswith('create_') for step, _, _ in f.calls))
        f = Fake(); f.overrides['schema_usage'] = SqlReply(True, '  schema_usage_allowed  ')
        f.summary = {'cases': list(PROFILES['primary_key_edges'][1]), 'passed': 5, 'failed': 0}
        self.assertEqual('qualified', self.edge_run(f)['phase'])
        query = next(query for step, query, _ in f.calls if step == 'schema_usage')
        self.assertEqual("SELECT CASE WHEN has_schema_privilege('odbcpp_pilot_test','odbcpp_fixture','USAGE') THEN 'schema_usage_allowed' ELSE 'schema_usage_blocked' END;", query)

    def test_edge_usage_exception_or_late_positive_preserves_no_creation(self):
        for late in (False, True):
            f = Fake(); original_sql = f.sql
            def sql(step, query, seconds):
                reply = original_sql(step, query, seconds)
                if step == 'schema_usage':
                    if late:
                        f.time = 181
                    else:
                        raise OSError('schema proof callback failed')
                return reply
            f.sql = sql
            result = self.edge_run(f)
            self.assertEqual('blocked', result['phase'])
            self.assertFalse(result['created'])
            self.assertFalse(result['uncertain_creations'])
            self.assertIsNone(result['cases'])
            self.assertTrue(result['activity_verified'])
            self.assertEqual(1, len([s for s, _, _ in f.calls if s == 'schema_usage']))
            self.assertFalse(any(s.startswith(('create_', 'grant_', 'drop_')) for s, _, _ in f.calls))

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
