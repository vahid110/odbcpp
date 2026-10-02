"""Read-only accounting assessment, never an amendment or execution permission.

Uses the locked v6 validator and its unchanged headroom/cap/horizon rules. Metric
records are caller-supplied observations, not authenticated AWS or billing proof.
No persistence, reservation, cloud calls, credential access or launcher exists.
"""
from decimal import Decimal

from tools.redshift import pilot_bootstrap as b
from tools.redshift import pilot_preflight as r0
from tools.redshift import pilot_recovery as r


def usage_observation(record, anchor, now):
    """Validate an observation's binding without turning its sum into spend."""
    e = r0._record(record, {'account_id', 'region', 'workgroup_id', 'namespace_id',
        'metric', 'dimension', 'unit', 'statistic', 'period_seconds', 'start', 'end',
        'observed_at', 'sample_count', 'reported_sum', 'billing_status',
        'classification'}, 'invalid_usage_observation')
    for key in ('account_id', 'region', 'workgroup_id', 'namespace_id'):
        r0._need(e[key] == getattr(anchor, key), 'usage_identity_mismatch')
    r0._need(e['metric'] == 'ChargedSeconds'
        and e['dimension'] == {'Workgroup': 'odbcpp-redshift-pilot'}
        and e['unit'] == 'Count' and e['statistic'] == 'Sum'
        and type(e['period_seconds']) is int and e['period_seconds'] == 60,
        'unsupported_usage_semantics')
    r0._need(e['classification'] == 'observation_only'
        and e['billing_status'] == 'DataUnavailableException', 'billing_not_reconciled')
    observed = r0._timestamp(e['observed_at'], now, Decimal(300), 'stale_usage_observation')
    start = b._time(e['start'], now); end = b._time(e['end'], now)
    r0._need(start == b._time(anchor.earliest_billable_start, now)
        and start < end <= observed and (observed-end).total_seconds() <= 300,
        'usage_coverage_mismatch')
    r0._need(type(e['sample_count']) is int and 0 < e['sample_count'] <= 1440,
        'invalid_usage_samples')
    amount = r0._number(e['reported_sum'], 'invalid_usage_sum')
    return {'classification': 'observation_only', 'reported_sum': str(amount),
        'billing_status': 'unavailable', 'actual_spend_usd': None,
        'remaining_allowance_usd': None, 'live_enabled': False,
        'unresolved': ['metric_units_and_aggregation', 'coverage_and_reporting_delay',
                       'storage_other_charges_and_tax', 'independent_provenance_review']}


def assess_candidate(session, current, expected_state_digest):
    """Assess one proposed window against existing rules without changing them.

The caller must hold continuation_session's canonical lock. A computable bound
still requires evidence review and an explicit versioned durable amendment.
"""
    result = {'status': 'blocked', 'live_enabled': False,
        'actual_spend_usd': None, 'remaining_allowance_usd': None}
    try:
        state = session._load(); now = session.clock()
        overlay = session._v2(state, now)
        raw = session._read_raw('setup.json')
        r0._need(r.digest(raw) == expected_state_digest and r.decode(raw) == state,
            'accounting_source_changed')
        r0._need(len(overlay['attempts']) == 2
            and all(x['phase'] == 'cleaned_pending_billing' for x in overlay['attempts']),
            'exhausted_clean_v6_required')
        session._fixed(current, now)
        deadline = b._time(current.hard_deadline, now, True)
        r0._need(deadline > now and deadline >= b._time(
            overlay['attempts'][-1]['anchor']['hard_deadline'], now, True),
            'invalid_candidate_deadline')
        result.update(source_state_digest=expected_state_digest,
            retained_compute_upper_bound_usd=overlay['compute_upper_bound_usd'],
            retained_total_upper_bound_usd=overlay['total_upper_bound_usd'])
        # Use unchanged existing limits: exhausted headroom is a finding, not
        # authority to widen the ceiling or reserve another paid window.
        compute, total = session._amounts(current.hard_deadline,
            r0._number(overlay['metering_seconds'], 'invalid_headroom') + 1200,
            r0._number(overlay['cleanup_seconds'], 'invalid_headroom') + 60,
            r0._number(overlay['compute_upper_bound_usd'], 'invalid_cumulative_bound'),
            r0._number(overlay['total_upper_bound_usd'], 'invalid_cumulative_bound'), now)
        result.update(candidate_compute_upper_bound_usd=str(compute),
            candidate_total_upper_bound_usd=str(total),
            reasons=['independent_evidence_review_and_explicit_amendment_required'])
    except r0.Blocked as error:
        result['reasons'] = [error.code]
    return result
