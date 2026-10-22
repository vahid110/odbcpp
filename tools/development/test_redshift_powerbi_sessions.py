"""Inert operational safety tests. No actual demand, DB or driver feature proof."""
from copy import deepcopy
import unittest

import redshift_powerbi_sessions as sessions

BASE_MS = 100000
EXEC_END = 1100000
CLEANUP_END = 2000000
SELF_BIRTH = '2026-10-22 12:00:00'


def new_state():
    return sessions.new_state(workload_id='qualification007-default-parallel',
                              input_profile_sha256='a' * 64, admitted_ms=BASE_MS,
                              execution_deadline_ms=EXEC_END, cleanup_deadline_ms=CLEANUP_END)


def measurement(demand=8, headroom=2, mode='default_parallel'):
    # Invented evidence references only. None is read, created or certified.
    return {'schema_version': 1, 'observation_kind': 'client_demand_with_server_census',
            'bound_kind': 'measured_simultaneous_request_upper_bound', 'parallel_mode': mode,
            'workload_id': 'qualification007-default-parallel', 'input_profile_sha256': 'a' * 64,
            'observed_at_ms': BASE_MS, 'measured_demand': demand, 'finite_headroom': headroom,
            'requested_cap': demand + headroom, 'headroom_justification': 'Synthetic explicit two-slot finite reserve',
            'client_demand': {'path': '/invented/client-demand.json', 'sha256': 'b' * 64},
            'server_census': {'path': '/invented/server-census.json', 'sha256': 'c' * 64},
            'review': {'path': '/invented/review.json', 'sha256': 'd' * 64}}


def census(cap='2', db_cap='32', rows=(), *, test_oid='11', admin_oid='10', db_oid='20',
           admin_cap='UNLIMITED', count=None):
    return {'USERS': [admin_oid + '|odbcpp_pilot_admin|t|' + admin_cap,
                      test_oid + '|odbcpp_pilot_test|f|' + cap],
            'DATABASE': [db_oid + '|odbcpp_pilot|' + db_cap],
            'COUNT': [str(len(rows) if count is None else count)], 'SESSIONS': list(rows)}


def encode(values):
    return ''.join('RSPBIS1|' + key + '\n' + ''.join(line + '\n' for line in rows)
                   for key, rows in values.items())


def payload(state, kind, *, rows=(), cap=None, db_cap=None):
    original = state['original'] or {'test_cap': cap or '2', 'database_cap': db_cap or '32'}
    base_cap = original['test_cap']
    database_cap = original['database_cap']
    result = {'SELF': ['999|odbcpp_pilot_admin|odbcpp_pilot|' + SELF_BIRTH]}
    if kind in sessions.MUTATIONS:
        pre_cap = base_cap if kind == 'set_cap' else str(state['temporary_cap'])
        post_cap = str(state['temporary_cap']) if kind == 'set_cap' else base_cap
        for prefix, current in [('PRE_', pre_cap), ('POST_', post_cap)]:
            result.update({prefix + key: value for key, value in census(current, database_cap).items()})
        result['ALTER_ACK' if kind == 'set_cap' else 'RESTORE_ACK'] = []
    else:
        current = cap if cap is not None else (str(state['temporary_cap']) if state['phase'] == 'CAP_ACTIVE' else base_cap)
        result.update(census(current, db_cap if db_cap is not None else database_cap, rows))
    return encode(result)


def advance(state, kind, *, now_ms=None, rows=(), cap=None, db_cap=None):
    if now_ms is None:
        now_ms = max(BASE_MS + 20, state['last_completed_ms'] + 1)
    intent = sessions.begin(state, sessions.stage_for(state, kind), now_ms=now_ms)
    return sessions.complete(intent, payload(state, kind, rows=rows, cap=cap, db_cap=db_cap),
                             native_exit=0, now_ms=now_ms + 1)


def decided(cap='2', db_cap='32', measured=None):
    state = advance(new_state(), 'snapshot', cap=cap, db_cap=db_cap)
    return sessions.decide(state, measurement() if measured is None else measured, now_ms=BASE_MS + 22)


def active():
    return advance(decided(), 'set_cap', now_ms=BASE_MS + 23)


def sample_rows(count, *, start=100, birth='2026-10-22 12:00:01'):
    return [str(pid) + '|odbcpp_pilot_test|odbcpp_pilot|' + birth for pid in range(start, start + count)]


class PowerBiSessionSafetyTests(unittest.TestCase):
    def test_cap_two_default_parallel_exact_change_and_restore(self):
        state = decided()
        self.assertEqual('2', state['original']['test_cap'])
        self.assertEqual(10, state['temporary_cap'])
        change = sessions.stage_for(state, 'set_cap')
        self.assertIn('ALTER USER "odbcpp_pilot_test" CONNECTION LIMIT 10;', change['query'])
        state = advance(state, 'set_cap', now_ms=BASE_MS + 23)
        for index, (event, count) in enumerate([('connected', 4), ('navigator', 8), ('refresh', 5)]):
            state = advance(state, 'sample_' + event, now_ms=BASE_MS + 30 + index * 3, rows=sample_rows(count))
        self.assertEqual(8, len(state['seen_sessions']))
        self.assertTrue(all(row['not_a_client_demand_measurement'] for row in state['observations']))
        state = advance(state, 'quiesce', now_ms=EXEC_END + 1)
        restore = sessions.stage_for(state, 'restore')
        self.assertIn('ALTER USER "odbcpp_pilot_test" CONNECTION LIMIT 2;', restore['query'])
        state = advance(state, 'restore', now_ms=EXEC_END + 5)
        self.assertEqual('CAP_RESTORED', state['phase'])
        self.assertEqual('2', state['original']['test_cap'])
        self.assertEqual(7, len(state['attempts']))

    def test_explicit_finite_headroom_not_forced_to_demand_or_sixteen(self):
        state = decided(measured=measurement(12, 4))
        self.assertEqual(16, state['temporary_cap'])
        state = decided(measured=measurement(5, 1, mode='explicit_parallel'))
        self.assertEqual(6, state['temporary_cap'])
        bad = measurement(8, 2)
        bad['requested_cap'] = 16
        with self.assertRaisesRegex(sessions.Refused, 'finite_demand_headroom'):
            decided(measured=bad)

    def test_physical_peak_trace_and_configured_max_are_not_demand(self):
        for observation in ('server_physical_peak', 'api_trace_pairs', 'configured_parallel_max'):
            value = measurement()
            value['observation_kind'] = observation
            with self.subTest(observation=observation), self.assertRaises(sessions.Refused):
                decided(measured=value)
        value = measurement()
        value['bound_kind'] = 'lower_bound_at_original_cap_2'
        with self.assertRaises(sessions.Refused):
            decided(measured=value)
        value = measurement()
        value['configured_parallel_limit'] = 16
        with self.assertRaises(sessions.Refused):
            decided(measured=value)
        state = advance(new_state(), 'snapshot')
        with self.assertRaises(sessions.Refused):
            sessions.stage_for(state, 'set_cap')
        with self.assertRaises(sessions.Refused):
            sessions.decide(state, None, now_ms=BASE_MS + 22)

    def test_only_exact_requested_cap_avoids_alter(self):
        for cap in ('2', '10', '32', 'UNLIMITED', '2147483647'):
            with self.subTest(cap=cap):
                state = decided(cap=cap, db_cap='-1')
                self.assertEqual(cap, state['original']['test_cap'])
                if cap == '10':
                    self.assertIsNone(state['temporary_cap'])
                    with self.assertRaises(sessions.Refused):
                        sessions.stage_for(state, 'set_cap')
                else:
                    self.assertEqual(10, state['temporary_cap'])
                    change = sessions.stage_for(state, 'set_cap')
                    self.assertIn('CONNECTION LIMIT 10;', change['query'])
                    state = advance(state, 'set_cap')
                state = advance(state, 'sample_connected', rows=sample_rows(8))
                state = advance(state, 'quiesce')
                if cap == '10':
                    self.assertEqual('CAP_VERIFIED_UNCHANGED', state['phase'])
                    with self.assertRaises(sessions.Refused):
                        sessions.stage_for(state, 'restore')
                else:
                    self.assertIn('CONNECTION LIMIT ' + cap + ';',
                                  sessions.stage_for(state, 'restore')['query'])
                    state = advance(state, 'restore')
                    self.assertEqual('CAP_RESTORED', state['phase'])
                    self.assertEqual(cap, state['original']['test_cap'])

    def test_zero_original_cap_restores_exact_zero(self):
        state = advance(decided(cap='0'), 'set_cap', now_ms=BASE_MS + 23)
        state = advance(state, 'quiesce')
        self.assertIn('CONNECTION LIMIT 0;', sessions.stage_for(state, 'restore')['query'])
        state = advance(state, 'restore')
        self.assertEqual('CAP_RESTORED', state['phase'])
        self.assertEqual('0', state['original']['test_cap'])

    def test_database_headroom_exact_boundary_and_no_database_change(self):
        with self.assertRaisesRegex(sessions.Refused, 'headroom'):
            decided(db_cap='10')
        state = decided(db_cap='11')
        self.assertEqual(10, state['temporary_cap'])
        for value in ('UNLIMITED', '-1', '2147483647'):
            with self.subTest(value=value):
                self.assertEqual(value, decided(db_cap=value)['original']['database_cap'])
        for value in ('0', '1'):
            with self.subTest(value=value), self.assertRaises(sessions.Refused):
                advance(new_state(), 'snapshot', db_cap=value)

    def test_snapshot_refuses_preexisting_and_unrelated_sessions(self):
        state = new_state()
        intent = sessions.begin(state, sessions.stage_for(state, 'snapshot'), now_ms=BASE_MS + 10)
        for row in [sample_rows(1)[0], '100|odbcpp_pilot_admin|odbcpp_pilot|' + SELF_BIRTH,
                    '100|foreign_user|odbcpp_pilot|' + SELF_BIRTH,
                    '100|odbcpp_pilot_test|foreign_database|' + SELF_BIRTH]:
            with self.subTest(row=row), self.assertRaises(sessions.Refused):
                sessions.complete(intent, payload(state, 'snapshot', rows=(row,)), native_exit=0, now_ms=BASE_MS + 11)

    def test_snapshot_malformed_caps_oids_superuser_and_database(self):
        state = new_state()
        intent = sessions.begin(state, sessions.stage_for(state, 'snapshot'), now_ms=BASE_MS + 10)
        good = payload(state, 'snapshot')
        changes = [('11|odbcpp_pilot_test|f|2', '11|odbcpp_pilot_test|f|2147483648'),
                   ('11|odbcpp_pilot_test|f|2', '11|odbcpp_pilot_test|f|-1'),
                   ('11|odbcpp_pilot_test|f|2', '11|odbcpp_pilot_test|f|02'),
                   ('11|odbcpp_pilot_test|f|2', '11|odbcpp_pilot_test|f|unlimited'),
                   ('11|odbcpp_pilot_test|f|2', '11|odbcpp_pilot_test|t|2'),
                   ('11|odbcpp_pilot_test|f|2', '4294967296|odbcpp_pilot_test|f|2'),
                   ('11|odbcpp_pilot_test|f|2', '10|odbcpp_pilot_test|f|2'),
                   ('20|odbcpp_pilot|32', '20|foreign_database|32'),
                   ('20|odbcpp_pilot|32', '20|odbcpp_pilot|-2')]
        for old, new in changes:
            with self.subTest(new=new), self.assertRaises(sessions.Refused):
                sessions.complete(intent, good.replace(old, new), native_exit=0, now_ms=BASE_MS + 11)

    def test_missing_ack_nonzero_and_late_alter_remain_unknown(self):
        state = decided()
        intent = sessions.begin(state, sessions.stage_for(state, 'set_cap'), now_ms=BASE_MS + 23)
        self.assertEqual('ALTER_UNKNOWN', intent['phase'])
        good = payload(state, 'set_cap')
        for data, exit_code, now in [(good.replace('RSPBIS1|ALTER_ACK\n', ''), 0, BASE_MS + 24),
                                     (good, 1, BASE_MS + 24), (good, False, BASE_MS + 24),
                                     (good, 0, EXEC_END), (good, 0, BASE_MS + 22)]:
            with self.subTest(exit=exit_code, now=now), self.assertRaises(sessions.Refused):
                sessions.complete(intent, data, native_exit=exit_code, now_ms=now)
            self.assertEqual('ALTER_UNKNOWN', intent['phase'])
            self.assertEqual('2', intent['original']['test_cap'])
        for kind in sessions.STAGES:
            with self.subTest(kind=kind), self.assertRaises(sessions.Refused):
                sessions.stage_for(intent, kind)

    def test_changed_mutation_identity_cap_or_readback_is_not_adopted(self):
        state = decided()
        intent = sessions.begin(state, sessions.stage_for(state, 'set_cap'), now_ms=BASE_MS + 23)
        good = payload(state, 'set_cap')
        changes = [('11|odbcpp_pilot_test|f|10', '12|odbcpp_pilot_test|f|10'),
                   ('11|odbcpp_pilot_test|f|10', '11|odbcpp_pilot_test|f|9'),
                   ('10|odbcpp_pilot_admin|t|UNLIMITED', '10|odbcpp_pilot_admin|t|2'),
                   ('20|odbcpp_pilot|32', '21|odbcpp_pilot|32'),
                   ('20|odbcpp_pilot|32', '20|odbcpp_pilot|33')]
        for old, new in changes:
            with self.subTest(new=new), self.assertRaises(sessions.Refused):
                sessions.complete(intent, good.replace(old, new), native_exit=0, now_ms=BASE_MS + 24)

    def test_measurement_identity_pins_freshness_and_finite_bounds(self):
        for key, value in [('workload_id', 'different-workload'), ('input_profile_sha256', 'e' * 64),
                           ('measured_demand', True), ('measured_demand', 0), ('measured_demand', 17),
                           ('requested_cap', 17), ('finite_headroom', -1),
                           ('headroom_justification', ''), ('observed_at_ms', BASE_MS + 50),
                           ('observed_at_ms', 1)]:
            bad = measurement()
            bad[key] = value
            # For the old measurement test choose an explicitly late decision.
            when = BASE_MS + 400000 if value == 1 and key == 'observed_at_ms' else BASE_MS + 22
            state = advance(new_state(), 'snapshot')
            with self.subTest(key=key, value=value), self.assertRaises(sessions.Refused):
                sessions.decide(state, bad, now_ms=when)
        for key in ('client_demand', 'server_census', 'review'):
            bad = measurement()
            bad[key] = {'path': '../foreign', 'sha256': 'not-a-digest'}
            with self.subTest(key=key), self.assertRaises(sessions.Refused):
                decided(measured=bad)
        bad = measurement()
        bad['server_census'] = deepcopy(bad['client_demand'])
        with self.assertRaises(sessions.Refused):
            decided(measured=bad)
        state = decided()
        with self.assertRaises(sessions.Refused):
            sessions.begin(state, sessions.stage_for(state, 'set_cap'), now_ms=BASE_MS + 400000)
        fresh = advance(new_state(), 'snapshot', now_ms=BASE_MS + 400000)
        with self.assertRaisesRegex(sessions.Refused, 'measurement_not_fresh'):
            sessions.decide(fresh, measurement(), now_ms=BASE_MS + 400002)

    def test_three_event_samples_fixed_bound_and_actual_saturation_label(self):
        state = advance(decided(measured=measurement(12, 4)), 'set_cap', now_ms=BASE_MS + 23)
        state = advance(state, 'sample_connected', rows=sample_rows(16))
        self.assertTrue(state['observations'][0]['at_partition_bound'])
        self.assertTrue(state['observations'][0]['not_a_client_demand_measurement'])
        with self.assertRaises(sessions.Refused):
            sessions.stage_for(state, 'sample_connected')
        with self.assertRaises(sessions.Refused):
            sessions.stage_for(state, 'sample_poll')
        self.assertEqual(16, len(state['seen_sessions']))

    def test_sample_overflow_race_duplicate_and_unrelated_refuse(self):
        state = active()
        stage = sessions.stage_for(state, 'sample_connected')
        intent = sessions.begin(state, stage, now_ms=BASE_MS + 30)
        good = payload(state, 'sample_connected', rows=sample_rows(2))
        bad_payloads = [payload(state, 'sample_connected', rows=sample_rows(17)),
                        payload(state, 'sample_connected', rows=sample_rows(11)),
                        good.replace('RSPBIS1|COUNT\n2', 'RSPBIS1|COUNT\n1'),
                        good.replace('101|odbcpp_pilot_test', '100|odbcpp_pilot_test'),
                        good.replace('100|odbcpp_pilot_test', '100|foreign_user'),
                        good.replace('100|odbcpp_pilot_test|odbcpp_pilot', '100|odbcpp_pilot_test|foreign_database'),
                        good.replace('100|odbcpp_pilot_test', '999|odbcpp_pilot_test'),
                        good.replace('12:00:01', '99:00:01')]
        for bad in bad_payloads:
            with self.subTest(data=bad[:80]), self.assertRaises(sessions.Refused):
                sessions.complete(intent, bad, native_exit=0, now_ms=BASE_MS + 31)

    def test_pid_birth_reuse_and_preexisting_birth_are_refused(self):
        state = advance(active(), 'sample_connected', rows=sample_rows(1))
        for birth in ('2026-10-22 12:00:02', '2026-10-22 11:59:59'):
            intent = sessions.begin(state, sessions.stage_for(state, 'sample_navigator'), now_ms=BASE_MS + 40)
            with self.subTest(birth=birth), self.assertRaises(sessions.Refused):
                sessions.complete(intent, payload(state, 'sample_navigator', rows=sample_rows(1, birth=birth)), native_exit=0, now_ms=BASE_MS + 41)

    def test_restore_only_after_zero_and_exact_original_readback(self):
        state = active()
        with self.assertRaises(sessions.Refused):
            sessions.stage_for(state, 'restore')
        intent = sessions.begin(state, sessions.stage_for(state, 'quiesce'), now_ms=EXEC_END + 1)
        with self.assertRaises(sessions.Refused):
            sessions.complete(intent, payload(state, 'quiesce', rows=sample_rows(1)), native_exit=0, now_ms=EXEC_END + 2)
        state = advance(state, 'quiesce', now_ms=EXEC_END + 1)
        intent = sessions.begin(state, sessions.stage_for(state, 'restore'), now_ms=EXEC_END + 5)
        self.assertEqual('RESTORE_UNKNOWN', intent['phase'])
        good = payload(state, 'restore')
        for bad, now in [(good.replace('RSPBIS1|RESTORE_ACK\n', ''), EXEC_END + 6),
                         (good.replace('11|odbcpp_pilot_test|f|2', '11|odbcpp_pilot_test|f|3'), EXEC_END + 6),
                         (good, CLEANUP_END)]:
            with self.subTest(now=now), self.assertRaises(sessions.Refused):
                sessions.complete(intent, bad, native_exit=0, now_ms=now)
            self.assertEqual('RESTORE_UNKNOWN', intent['phase'])

    def test_original_deadlines_not_replaced_by_stage_or_cleanup(self):
        state = decided()
        stage = sessions.stage_for(state, 'set_cap')
        intent = sessions.begin(state, stage, now_ms=BASE_MS + 23)
        self.assertEqual(EXEC_END, intent['pending']['deadline_ms'])
        changed = deepcopy(intent)
        changed['pending']['deadline_ms'] = CLEANUP_END
        with self.assertRaises(sessions.Refused):
            sessions.complete(changed, payload(state, 'set_cap'), native_exit=0, now_ms=BASE_MS + 24)
        with self.assertRaises(sessions.Refused):
            sessions.begin(state, stage, now_ms=EXEC_END)
        state = advance(active(), 'quiesce', now_ms=EXEC_END + 1)
        intent = sessions.begin(state, sessions.stage_for(state, 'restore'), now_ms=EXEC_END + 5)
        self.assertEqual(CLEANUP_END, intent['pending']['deadline_ms'])

    def test_closed_sql_targets_no_password_reads_no_other_mutations(self):
        state = new_state()
        total_sql = 0
        kinds = ['snapshot', 'set_cap', *(('sample_' + event) for event in sessions.EVENTS), 'quiesce', 'restore']
        for index, kind in enumerate(kinds):
            stage = sessions.stage_for(state, kind)
            query = stage['query']
            self.assertEqual('admin', stage['role'])
            self.assertLessEqual(len(query.encode('utf-8')), 16384)
            self.assertIn('usesysid,TRIM(usename),usesuper,TRIM(useconnlimit)', query)
            self.assertIn('datid,TRIM(datname),TRIM(datconnlimit)', query)
            self.assertIn('starttime::VARCHAR', query)
            self.assertIn('LIMIT 17', query)
            for forbidden in ('SELECT *', 'passwd', 'password', 'ALTER DATABASE', 'GRANT ', 'CREATE USER', 'pg_terminate_backend'):
                self.assertNotIn(forbidden, query)
            if kind in sessions.MUTATIONS:
                self.assertEqual(1, query.count('ALTER USER "odbcpp_pilot_test" CONNECTION LIMIT '))
                self.assertIn('AND usesysid=', query.replace('WHERE usesysid=', 'AND usesysid='))
                self.assertEqual(2, query.count('\\if :pbi_sessions_held'))
                self.assertEqual(2, query.count('\\endif'))
                self.assertLess(query.index('ALTER USER'), query.index('ACK'))
            else:
                self.assertNotIn('ALTER USER', query)
            total_sql += stage['statements'] + 3
            state = advance(state, kind, now_ms=BASE_MS + 30 + index * 3)
            if kind == 'snapshot':
                state = sessions.decide(state, measurement(), now_ms=BASE_MS + 32)
        plan = sessions.public_plan()
        self.assertEqual(70, total_sql)
        self.assertEqual(77, total_sql + len(kinds))
        self.assertEqual(77, plan['maximum_changed_path']['inclusive'])
        self.assertEqual(40, plan['existing_global_guards_unchanged']['helpers_including_network'])
        self.assertEqual(450, plan['existing_global_guards_unchanged']['SQL_plus_logins'])
        self.assertTrue(plan['existing_cleanup_max8_insufficient_for16'])

    def test_unchanged_path_cost_is_counted_too(self):
        state = new_state()
        total_sql = 0
        for kind in ['snapshot', *(('sample_' + event) for event in sessions.EVENTS), 'quiesce']:
            total_sql += sessions.stage_for(state, kind)['statements'] + 3
            state = advance(state, kind, cap='10' if kind == 'snapshot' else None)
            if kind == 'snapshot':
                state = sessions.decide(state, measurement(), now_ms=BASE_MS + 22)
        self.assertEqual('CAP_VERIFIED_UNCHANGED', state['phase'])
        self.assertEqual(40, total_sql)
        self.assertEqual(45, total_sql + len(state['attempts']))

    def test_modified_query_role_keys_or_intent_cannot_dispatch_or_complete(self):
        state = decided()
        stage = sessions.stage_for(state, 'set_cap')
        for field, value in [('query', 'ALTER USER foreign_user CONNECTION LIMIT 16;'),
                             ('statements', 0), ('role', 'test'), ('keys', ('FOREIGN',))]:
            with self.subTest(field=field), self.assertRaises(sessions.Refused):
                sessions.begin(state, {**stage, field: value}, now_ms=BASE_MS + 23)
        intent = sessions.begin(state, stage, now_ms=BASE_MS + 23)
        intent['phase'] = 'CAP_ACTIVE'
        with self.assertRaises(sessions.Refused):
            sessions.complete(intent, payload(state, 'set_cap'), native_exit=0, now_ms=BASE_MS + 24)

    def test_exact_frames_unknowns_duplicate_or_overflow_refuse(self):
        for payload_text in ['foreign', 'RSPBIS1|SELF\nRSPBIS1|SELF\n',
                             'RSPBIS1|SESSIONS\n' + '1\n' * 18, '\x00', 'x' * 32769]:
            with self.subTest(data=payload_text[:40]), self.assertRaises(sessions.Refused):
                sessions.parse_frames(payload_text)
        state = decided()
        intent = sessions.begin(state, sessions.stage_for(state, 'set_cap'), now_ms=BASE_MS + 23)
        good = payload(state, 'set_cap')
        for suffix in ('RSPBIS1|REFUSED\n', 'RSPBIS1|UNKNOWN\n'):
            with self.subTest(suffix=suffix), self.assertRaises(sessions.Refused):
                sessions.complete(intent, good + suffix, native_exit=0, now_ms=BASE_MS + 24)


if __name__ == '__main__':
    unittest.main()
