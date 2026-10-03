"""Fixed, injected fixture lifecycle for provisioned catalog GoogleTests.

No launcher, credentials, cloud calls or budget authority. The owner holds the
canonical lock, validates fresh controls/artifact/accounting, and supplies durable
consumption/events and bounded SQL/GTest callbacks. Feature assertions stay in
it_redshift_real.cpp. This helper never invokes the fixture procedure.
"""
from dataclasses import dataclass
import hashlib
import math
from pathlib import Path

SCHEMA = 'odbcpp_fixture'
CREATOR = 'odbcpp_pilot_admin'
PRINCIPAL = 'odbcpp_pilot_test'
DATABASE = 'odbcpp_pilot'
PARENT = 'm2_catalog_parent_20261003_c01'
CHILD = 'm2_catalog_child_20261003_c01'
PROCEDURE = 'sp_m2_catalog_modes_20261003_c01'
OBJECTS = (PARENT, CHILD, PROCEDURE)
CASES = tuple('RedshiftRealTest.' + n for n in (
    'ConnectionTest', 'CompositePrimaryKeyCatalogContract',
    'CompositeForeignKeyCatalogContract', 'ProcedureCatalogContract',
    'ProcedureParameterCatalogContract'))
OWNED = "user_name IN ('odbcpp_pilot_admin','odbcpp_pilot_test') AND db_name='odbcpp_pilot'"


@dataclass(frozen=True)
class SqlReply:
    succeeded: bool
    output: str = ''


class FixtureBlocked(RuntimeError):
    pass


def statements(path: Path, digest: str):
    if path.is_symlink():
        raise FixtureBlocked('fixture_changed')
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise FixtureBlocked('fixture_changed')
    text = data.decode('utf-8')
    result = []
    for name in (PARENT, CHILD):
        start = text.index('CREATE TABLE ' + SCHEMA + '.' + name + ' (')
        end = text.index(';', start) + 1
        result.append(text[start:end])
    start = text.index('CREATE PROCEDURE ' + SCHEMA + '.' + PROCEDURE + ' (')
    end = text.index('$$ LANGUAGE plpgsql SECURITY INVOKER;', start)
    result.append(text[start:end] + '$$ LANGUAGE plpgsql SECURITY INVOKER;')
    return tuple(result)


class CatalogFixture:
    def __init__(self, *, sql, tests, record, clock, deadline, cleanup_deadline,
                 fixture, fixture_digest):
        self.sql, self.tests, self.record, self.clock = sql, tests, record, clock
        self.deadline, self.cleanup_deadline = deadline, cleanup_deadline
        self.fixture, self.fixture_digest = fixture, fixture_digest
        self.cleanup_cutoff = cleanup_deadline

    def _sql(self, step, query, cleanup=False):
        cutoff = self.cleanup_cutoff if cleanup else self.deadline
        remaining = cutoff - self.clock()
        if remaining <= 0:
            raise FixtureBlocked('deadline_exhausted')
        self.record({'step': step, 'event': 'dispatch'})
        remaining = cutoff - self.clock()
        if remaining <= 0:
            raise FixtureBlocked('deadline_exhausted')
        reply = self.sql(step, query, min(15, remaining))
        if self.clock() > cutoff:
            raise FixtureBlocked('late_sql_acknowledgment')
        if not isinstance(reply, SqlReply) or reply.succeeded is not True:
            raise FixtureBlocked('sql_step_failed')
        self.record({'step': step, 'event': 'acknowledged'})
        if self.clock() > cutoff:
            raise FixtureBlocked('late_sql_acknowledgment')
        return reply.output.strip()

    def _activity(self):
        # Fetch pids first; provisioned Redshift rejects termination functions
        # embedded in queries over STV_SESSIONS. Never broaden owned principals.
        output = self._sql('cleanup_pids',
            'SELECT process FROM stv_sessions WHERE ' + OWNED +
            ' AND process <> pg_backend_pid();', True)
        pids = output.splitlines() if output else []
        if len(pids) > 8 or len(pids) != len(set(pids)) or any(
                not pid.isascii() or not pid.isdigit() or not 0 < int(pid) <= 2147483647
                for pid in pids):
            raise FixtureBlocked('invalid_owned_pid_inventory')
        for pid in pids:
            if self._sql('terminate_' + pid, 'SELECT pg_terminate_backend(' + pid + ');', True) != 't':
                raise FixtureBlocked('termination_unverified')
        for step, table, extra in (('cleanup_sessions', 'stv_sessions', 'process'),
                                  ('cleanup_queries', 'stv_recents', 'pid')):
            query = 'SELECT EXISTS(SELECT 1 FROM ' + table + ' WHERE ' + OWNED
            if table == 'stv_recents':
                query += " AND status <> 'Done'"
            query += ' AND ' + extra + ' <> pg_backend_pid());'
            if self._sql(step, query, True) != 'f':
                raise FixtureBlocked('remote_activity_unverified')

    def run(self):
        now = self.clock()
        if any(type(n) not in (int, float) or not math.isfinite(n)
               for n in (now, self.deadline, self.cleanup_deadline)) or not (
                0 < self.deadline - now <= 180 and
                0 < self.cleanup_deadline - self.deadline <= 60):
            raise FixtureBlocked('invalid_absolute_deadlines')
        ddl = statements(self.fixture, self.fixture_digest)
        # Callback must fsync consumed admission before returning. No SQL if it fails.
        self.record({'event': 'consumed', 'cases': list(CASES), 'objects': list(OBJECTS)})
        created, uncertain = [], []
        identity_verified = False
        result = {'phase': 'blocked', 'cases': None, 'objects_cleaned': False,
                  'activity_verified': False}
        try:
            if self._sql('identity', 'SELECT current_database(),TRIM(current_user);') != DATABASE + '|' + CREATOR:
                raise FixtureBlocked('identity_mismatch')
            identity_verified = True
            if self._sql('collision_tables', "SELECT COUNT(*) FROM pg_class c JOIN pg_namespace n ON c.relnamespace=n.oid WHERE n.nspname='odbcpp_fixture' AND c.relname IN ('" + PARENT + "','" + CHILD + "');") != '0':
                raise FixtureBlocked('fixture_collision')
            if self._sql('collision_procedures', "SELECT COUNT(*) FROM pg_proc_info p JOIN pg_namespace n ON p.pronamespace=n.oid WHERE n.nspname='odbcpp_fixture' AND p.proname='" + PROCEDURE + "';") != '0':
                raise FixtureBlocked('fixture_collision')
            for name, command in zip(OBJECTS, ddl):
                try:
                    self._sql('create_' + name, command)
                except Exception:
                    uncertain.append(name)
                    raise
                created.append(name)
                self.record({'event': 'created', 'object': name})
                grant = ('GRANT EXECUTE ON PROCEDURE ' + SCHEMA + '.' + name +
                    '(INTEGER,INTEGER) TO ' + PRINCIPAL + ';') if name == PROCEDURE else (
                    'GRANT SELECT ON ' + SCHEMA + '.' + name + ' TO ' + PRINCIPAL + ';')
                self._sql('grant_' + name, grant)
                self.record({'event': 'granted', 'object': name})
            remaining = self.deadline - self.clock()
            if remaining <= 0:
                raise FixtureBlocked('deadline_exhausted')
            summary = self.tests(CASES, remaining)
            if self.clock() > self.deadline:
                raise FixtureBlocked('late_gtest_result')
            if (not isinstance(summary, dict) or type(summary.get('cases')) not in (tuple, list)
                    or len(summary['cases']) != len(CASES)
                    or len(set(summary['cases'])) != len(CASES) or set(summary['cases']) != set(CASES)
                    or type(summary.get('passed')) is not int or type(summary.get('failed')) is not int
                    or min(summary['passed'], summary['failed']) < 0
                    or summary['passed'] + summary['failed'] != len(CASES)):
                raise FixtureBlocked('invalid_gtest_inventory')
            result['cases'] = summary
            result['phase'] = 'qualified' if summary['failed'] == 0 else 'qualification_failed'
        except Exception as error:
            result['reason'] = str(error) if isinstance(error, FixtureBlocked) else 'callback_failed'
        finally:
            self.cleanup_cutoff = min(self.cleanup_deadline, self.clock() + 60)
            try:
                if not identity_verified:
                    raise FixtureBlocked('cleanup_identity_unverified')
                self._activity()
                for name in reversed(created if not uncertain else []):
                    if name == PROCEDURE:
                        query = "SELECT u.usename,p.prokind,p.pronargs,p.proargtypes[0],p.proargtypes[1] FROM pg_proc_info p JOIN pg_namespace n ON p.pronamespace=n.oid JOIN pg_user u ON p.proowner=u.usesysid WHERE n.nspname='odbcpp_fixture' AND p.proname='" + name + "';"
                        expected = CREATOR + '|p|2|23|23'
                        drop = 'DROP PROCEDURE ' + SCHEMA + '.' + name + '(INTEGER,INTEGER);'
                    else:
                        query = "SELECT u.usename,c.relkind FROM pg_class c JOIN pg_namespace n ON c.relnamespace=n.oid JOIN pg_user u ON c.relowner=u.usesysid WHERE n.nspname='odbcpp_fixture' AND c.relname='" + name + "';"
                        expected = CREATOR + '|r'
                        drop = 'DROP TABLE ' + SCHEMA + '.' + name + ' RESTRICT;'
                    owner = '|'.join(part.strip() for part in self._sql('owner_' + name, query, True).split('|'))
                    if owner != expected:
                        raise FixtureBlocked('fixture_ownership_unverified')
                    self._sql('drop_' + name, drop, True)
                    self.record({'event': 'dropped', 'object': name})
                self._activity()
                result['activity_verified'] = True
                result['objects_cleaned'] = not uncertain
            except Exception:
                result['cleanup_reason'] = 'cleanup_unverified'
            if not result['activity_verified'] or not result['objects_cleaned']:
                result['qualification_phase'] = result['phase']
                result['phase'] = 'cleanup_unverified'
            result['created'] = created
            result['uncertain_creations'] = uncertain
            self.record({'event': 'finished', 'result': result})
        return result
