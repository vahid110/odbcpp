"""Closed qualification SQL for Serverless activity, never an executor.

Resolve the two exact principals in a separate leader statement before reading
SYS views. SYS_SESSION_HISTORY.user_id is char(50), unlike query user_id. Active
session birth timestamps are preserved; no history-age cutoff substitutes for
current ownership. Defaults in callers retain the provisioned STV contract.

AWS SYS_SESSION_HISTORY and SYS_QUERY_HISTORY documentation defines these
columns and the active/planning/queued/running/returning states.
"""

DATABASE = 'odbcpp_pilot'
ADMIN = 'odbcpp_pilot_admin'
TEST = 'odbcpp_pilot_test'


def binding(*, admin_oid=None, test_oid=None):
    """One counted read, fail closed before any following SYS/termination SQL."""
    if (admin_oid is None) != (test_oid is None):
        raise ValueError('principal_oid_pair')
    held = ''
    if admin_oid is not None:
        values = (admin_oid, test_oid)
        if not all(type(value) is int and 0 < value <= 4294967295 for value in values) or admin_oid == test_oid:
            raise ValueError('principal_oid_binding')
        held = (" AND MAX(CASE WHEN usename='odbcpp_pilot_admin' THEN usesysid END)=" + str(admin_oid) +
                " AND MAX(CASE WHEN usename='odbcpp_pilot_test' THEN usesysid END)=" + str(test_oid))
    return ("SELECT MAX(CASE WHEN usename='odbcpp_pilot_admin' AND usesuper THEN usesysid END) AS rs_admin_oid,"
            "MAX(CASE WHEN usename='odbcpp_pilot_test' AND NOT usesuper THEN usesysid END) AS rs_test_oid,"
            "CASE WHEN COUNT(*)=2 AND COUNT(DISTINCT usesysid)=2 AND "
            "COUNT(CASE WHEN usename='odbcpp_pilot_admin' AND usesuper THEN 1 END)=1 AND "
            "COUNT(CASE WHEN usename='odbcpp_pilot_test' AND NOT usesuper THEN 1 END)=1" + held + " "
            "THEN 1 ELSE 0 END AS rs_principals_held FROM pg_user_info "
            "WHERE usename IN ('odbcpp_pilot_admin','odbcpp_pilot_test');\n"
            "\\gset\n\\if :rs_principals_held\n\\else\n\\quit 4\n\\endif\n")


def user_id():
    return "TRY_CAST(NULLIF(TRIM(user_id),'') AS BIGINT)"


def name():
    return ("CASE " + user_id() + " WHEN :rs_admin_oid THEN '" + ADMIN +
            "' WHEN :rs_test_oid THEN '" + TEST + "' ELSE '<unrelated>' END")


def scope(*, queries=False, broad=False, self_only=False):
    uid = 'user_id' if queries else user_id()
    owned = uid + ' IN (:rs_admin_oid,:rs_test_oid)'
    db = "TRIM(database_name)='" + DATABASE + "'"
    identity = '(' + db + (' OR ' if broad else ' AND ') + owned + ')'
    state = ("status IN ('planning','queued','running','returning')" if queries
             else "status='active'")
    return identity + ' AND ' + state + ' AND session_id' + ('=' if self_only else '<>') + 'pg_backend_pid()'


def rows(*, broad=False, self_only=False, limit=9):
    if type(limit) is not int or limit not in (2, 4, 9, 17):
        raise ValueError('session_row_limit')
    return ('SELECT session_id,' + name() + ',TRIM(database_name),start_time::VARCHAR '
            'FROM sys_session_history WHERE ' + scope(broad=broad, self_only=self_only) +
            ' ORDER BY session_id LIMIT ' + str(limit))


def exists_sql(*, queries=False):
    view = 'sys_query_history' if queries else 'sys_session_history'
    return 'SELECT EXISTS(SELECT 1 FROM ' + view + ' WHERE ' + scope(queries=queries) + ')'


def zero():
    # Independent SYS subqueries exclude the helper; neither history table is
    # joined to the other, nor to leader-only principal catalogs.
    return ('SELECT EXISTS(SELECT 1 FROM sys_session_history WHERE ' + scope() +
            ') AS sessions,EXISTS(SELECT 1 FROM sys_query_history WHERE ' + scope(queries=True) +
            ') AS queries;')
