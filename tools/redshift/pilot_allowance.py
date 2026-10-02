"""Explicit USD150 authorization overlay; no SQL admission or live launcher.

Preserves and validates the complete USD15 v2 history in the same locked ledger.
Unknown charges remain unknown. Increasing permission never releases a previous
reservation. Cloud limits are verified separately, after durable migration.
Old launchers reject v3; a future reviewed launcher must explicitly support it.
Authorization evidence is operator-trusted, not authenticated by this module.
"""
import copy
from contextlib import contextmanager
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_cumulative as c
from tools.redshift import pilot_preflight as r0

AUTHORIZATION_TEXT = 'User: you can increase redshift allowance from 15 to 150$'
AUTHORIZATION_KEYS = {'record_type', 'recorded_at', 'account_id',
    'total_initial_allowance_usd', 'previous_total_initial_allowance_usd',
    'scope', 'authorization', 'capacity_change_authorized',
    'resource_expansion_authorized', 'implementation_status',
    'planning_compute_envelope_usd', 'planning_other_tax_reserve_usd'}


def _authorization_time(value, now):
    # Preserve the exact saved authorization; the operator record used +00:00.
    if isinstance(value, str) and value.endswith('+00:00'):
        value = value[:-6] + 'Z'
    return b._time(value, now)


class AllowanceSession(b.BootstrapSession):
    def _authorization(self, authorization, now):
        a = r0._record(authorization, AUTHORIZATION_KEYS, 'invalid_authorization')
        r0._need(a['record_type'] == 'user_allowance_amendment'
            and a['account_id'] == self.anchor.account_id
            and a['authorization'] == AUTHORIZATION_TEXT
            and a['total_initial_allowance_usd'] == '150'
            and a['previous_total_initial_allowance_usd'] == '15'
            and a['planning_compute_envelope_usd'] == '100'
            and a['planning_other_tax_reserve_usd'] == '50'
            and a['capacity_change_authorized'] is False
            and a['resource_expansion_authorized'] is False
            and a['implementation_status'] == 'recorded_pending_reviewed_accounting_and_aws_control_migration'
            and a['scope'] == 'One initial Redshift testing allowance shared across all chats; not monthly or per chat. Prior costs and reservations count toward this total.',
            'authorization_mismatch')
        recorded = _authorization_time(a['recorded_at'], now)
        r0._need(recorded >= b._time(self.anchor.earliest_billable_start, now),
                 'authorization_time_mismatch')
        return a

    def _previous(self, state, previous, now):
        historical = dict(state)
        historical[b.OVERLAY] = previous
        # Validate with the original class: USD150 rules must never reinterpret
        # historical USD15 arithmetic or allow a previously invalid history.
        validator = c.CumulativeSession(self.fd, self.directory, self.anchor, self.clock)
        try:
            o = validator._v2(historical, now)
        finally:
            validator.closed = True
        r0._need(not o['attempts'] or o['attempts'][-1]['phase'] == 'cleaned_pending_billing',
                 'prior_attempt_unresolved')
        return o

    def _observation(self, evidence, now, *, fresh):
        e = r0._record(evidence, {'verified', 'observed_at', 'account_id', 'region',
            'workgroup_id', 'namespace_id', 'base_rpus', 'max_rpus',
            'workgroup_count', 'namespace_count', 'budget_total_usd',
            'budget_start', 'budget_end', 'budget_alerts_usd', 'usage_amount_rpu_hours',
            'usage_period', 'usage_type', 'usage_action', 'usd_per_rpu_hour',
            'require_ssl', 'max_query_execution_seconds', 'network_verified'},
            'invalid_control_evidence')
        r0._need(e['verified'] is True and e['network_verified'] is True
            and e['account_id'] == self.anchor.account_id and e['region'] == self.anchor.region
            and e['workgroup_id'] == self.anchor.workgroup_id
            and e['namespace_id'] == self.anchor.namespace_id
            and e['require_ssl'] is True and e['usage_period'] == 'monthly'
            and e['usage_type'] == 'serverless-compute' and e['usage_action'] == 'deactivate'
            and e['budget_start'] == '2026-10-02' and e['budget_end'] == '2026-10-12'
            and e['budget_alerts_usd'] == ['100', '125', '150'], 'controls_mismatch')
        for key, value in (('base_rpus',4), ('max_rpus',4), ('workgroup_count',1),
            ('namespace_count',1), ('budget_total_usd',150), ('usage_amount_rpu_hours',267),
            ('usd_per_rpu_hour',Decimal('.374')), ('max_query_execution_seconds',30)):
            r0._need(r0._number(e[key], 'invalid_control_evidence') == value, 'controls_mismatch')
        if fresh:
            r0._timestamp(e['observed_at'], now, Decimal(300), 'stale_controls')
        else:
            b._time(e['observed_at'], now)
        return e

    def _v3(self, state, now):
        o = r0._record(state.get(b.OVERLAY), {'schema_version', 'previous_overlay',
            'previous_digest', 'legacy_digest', 'authorization', 'authorization_digest',
            'amended_at', 'compute_limit_usd', 'total_limit_usd', 'other_tax_reserve_usd',
            'compute_upper_bound_usd', 'total_upper_bound_usd', 'controls_status',
            'control_observation'}, 'invalid_allowance_overlay')
        r0._need(type(o['schema_version']) is int and o['schema_version'] == 3
            and o['legacy_digest'] == b._legacy_digest(state)
            and o['previous_digest'] == c._digest(o['previous_overlay'])
            and o['authorization_digest'] == c._digest(o['authorization']), 'allowance_history_mismatch')
        prior = self._previous(state, o['previous_overlay'], now)
        a = self._authorization(o['authorization'], now)
        amended = b._time(o['amended_at'], now)
        last_cleanup = prior['attempts'][-1]['cleanup_at'] if prior['attempts'] else prior['migration_cleanup_at']
        r0._need(amended >= _authorization_time(a['recorded_at'], now)
            and amended >= b._time(last_cleanup, now), 'amendment_time_mismatch')
        compute = r0._number(prior['compute_upper_bound_usd'], 'invalid_bound')
        total = max(r0._number(prior['total_upper_bound_usd'], 'invalid_bound'), compute+50)
        for key, value in (('compute_limit_usd',100), ('total_limit_usd',150),
            ('other_tax_reserve_usd',50), ('compute_upper_bound_usd',compute),
            ('total_upper_bound_usd',total)):
            r0._need(r0._number(o[key], 'invalid_bound') == value, 'allowance_history_mismatch')
        r0._need(compute <= 100 and total <= 150, 'budget_exhausted')
        r0._need(o['controls_status'] in ('pending', 'verified'), 'invalid_control_state')
        if o['controls_status'] == 'pending':
            r0._need(o['control_observation'] is None, 'invalid_control_state')
        else:
            e = self._observation(o['control_observation'], now, fresh=False)
            r0._need(b._time(e['observed_at'], now) >= amended, 'control_time_mismatch')
        return o

    def amend(self, authorization):
        def change(state, now):
            prior = self._previous(state, state.get(b.OVERLAY), now)
            a = self._authorization(authorization, now)
            last = prior['attempts'][-1]['cleanup_at'] if prior['attempts'] else prior['migration_cleanup_at']
            r0._need(_authorization_time(a['recorded_at'], now) >= b._time(last, now),
                     'authorization_time_mismatch')
            compute = r0._number(prior['compute_upper_bound_usd'], 'invalid_bound')
            total = max(r0._number(prior['total_upper_bound_usd'], 'invalid_bound'), compute+50)
            state[b.OVERLAY] = dict(schema_version=3, previous_overlay=copy.deepcopy(prior),
                previous_digest=c._digest(prior), legacy_digest=b._legacy_digest(state),
                authorization=copy.deepcopy(a), authorization_digest=c._digest(a),
                amended_at=now.isoformat().replace('+00:00','Z'), compute_limit_usd='100',
                total_limit_usd='150', other_tax_reserve_usd='50', compute_upper_bound_usd=str(compute),
                total_upper_bound_usd=str(total), controls_status='pending', control_observation=None)
            self._v3(state, now)
        return self._operate(change)

    def verify_controls(self, evidence):
        def change(state, now):
            o = self._v3(state, now)
            r0._need(o['controls_status'] == 'pending', 'controls_already_recorded')
            e = self._observation(evidence, now, fresh=True)
            r0._need(b._time(e['observed_at'], now) >= b._time(o['amended_at'], now),
                     'control_time_mismatch')
            o.update(controls_status='verified', control_observation=copy.deepcopy(e))
            self._v3(state, now)
        return self._operate(change)


@contextmanager
def allowance_session(path, original_anchor, *, clock=None):
    path = Path(path)
    r0._need(path.name == 'setup.json', 'invalid_state_path')
    with r0.pilot_lock(path.parent) as fd:
        session = AllowanceSession(fd, path.parent, original_anchor,
                                  clock or (lambda: datetime.now(timezone.utc)))
        try:
            yield session
        finally:
            session.closed = True
