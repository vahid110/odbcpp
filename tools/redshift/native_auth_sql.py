"""Psql-only callback for the fixed native AUTH remote cleanup statements.

Caller supplies already protected config/executable selection, separately checks
control admission/outer bounds. Each invocation additionally requires its own
exact effective database/admin and superuser guard before the selected SQL. No config/credential
loader or CLI. Root supplies an explicit protected passfile path; no password values are
accepted in config, argv, environment or diagnostics.
Python cannot interrupt a stalled OS process creation; timeout kills/reaps the
direct psql child once started, not its remote sessions. Remote cleanup remains
separate. No shell, psql startup file, default service/provider or retry is used.
"""
import math
from pathlib import Path
import re
import subprocess
import time

from .native_auth_cleanup import DATABASE, SqlReply, _PRINCIPAL
from .pilot_runtime import verify_executable
from .pilot_preflight import validate_private_file


_ALLOWED = b'native_auth_guard_allowed\n'


class SqlBlocked(RuntimeError):
    pass


class NativeAuthSql:
    def __init__(self, *, config, principal, executable, expected_sha256, clock=time.monotonic):
        required = {'host', 'port', 'database', 'user', 'passfile', 'ca_file'}
        if (not callable(clock) or type(config) is not dict or set(config) != required or
                type(principal) is not str or not _PRINCIPAL.fullmatch(principal) or
                type(executable) is not str or not executable.startswith('/') or
                type(expected_sha256) is not str or
                not re.fullmatch(r'[0-9a-f]{64}', expected_sha256)):
            raise SqlBlocked('invalid_selection') from None
        host, user, ca = config['host'], config['user'], config['ca_file']
        if (type(host) is not str or not 1 <= len(host) <= 253 or any(
                not re.fullmatch(r'[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?', label)
                for label in host.split('.')) or
                type(config['port']) is not int or not 1 <= config['port'] <= 65535 or
                type(config['database']) is not str or config['database'] != DATABASE or
                type(user) is not str or not _PRINCIPAL.fullmatch(user) or
                type(ca) is not str or not ca.startswith('/') or not 1 <= len(ca) <= 4096 or
                any(ord(c) < 32 or ord(c) == 127 for c in ca) or
                type(config['passfile']) is not str or not config['passfile'].startswith('/') or
                not 1 <= len(config['passfile']) <= 4096 or
                any(ord(c) < 32 or ord(c) == 127 for c in config['passfile'])):
            raise SqlBlocked('invalid_connection') from None
        self._config, self._principal = dict(config), principal
        self._executable, self._sha256 = executable, expected_sha256
        self._clock, self._highwater, self._clock_failed = clock, None, False
        self._busy = False

    def _now(self):
        if self._clock_failed:
            raise SqlBlocked('clock_failed') from None
        try:
            now = self._clock()
            valid = type(now) in (int, float) and math.isfinite(now) and now >= 0
        except Exception:
            valid = False
        if self._clock_failed:
            raise SqlBlocked('clock_failed') from None
        if not valid or (self._highwater is not None and now < self._highwater):
            self._clock_failed = True
            raise SqlBlocked('clock_failed') from None
        self._highwater = now
        return now

    def _selected_query(self, step, query):
        if type(step) is not str or type(query) is not str:
            raise SqlBlocked('invalid_statement') from None
        def predicate(column):
            return ("user_name='" + self._principal + "' AND db_name='" + DATABASE +
                    "' AND " + column + ' <> pg_backend_pid()')
        statements = {
            'preflight_sessions': 'SELECT EXISTS(SELECT 1 FROM stv_sessions WHERE ' + predicate('process') + ');',
            'cleanup_sessions': 'SELECT EXISTS(SELECT 1 FROM stv_sessions WHERE ' + predicate('process') + ');',
            'preflight_queries': 'SELECT EXISTS(SELECT 1 FROM stv_recents WHERE ' + predicate('pid') + " AND status <> 'Done');",
            'cleanup_queries': 'SELECT EXISTS(SELECT 1 FROM stv_recents WHERE ' + predicate('pid') + " AND status <> 'Done');",
            'cleanup_pids': 'SELECT process FROM stv_sessions WHERE ' + predicate('process') + ';',
        }
        if step in statements and query == statements[step]:
            # Nine rows suffice to refuse a >8 inventory without fetching it all.
            return query[:-1] + ' LIMIT 9;' if step == 'cleanup_pids' else query
        match = re.fullmatch(r'terminate_([1-9][0-9]{0,9})', step, re.ASCII)
        if (match and int(match[1]) <= 2147483647 and
                query == 'SELECT pg_terminate_backend(' + match[1] + ');'):
            return query
        raise SqlBlocked('invalid_statement') from None

    def _guarded_command(self, selected, milliseconds):
        # psql17 gset sends the unterminated buffer exactly once; if skips SQL
        # on false/malformed variables. CASE returns one non-NULL fixed value.
        # Redshift PG_USER usesuper is required for full STV row visibility.
        guard = ("SELECT CASE WHEN current_database()='" + DATABASE +
                 "' AND TRIM(current_user)='" + self._config['user'] +
                 "' AND EXISTS(SELECT 1 FROM pg_user WHERE usename=current_user AND usesuper) "
                 "THEN 'true' ELSE 'false' END AS native_auth_guard")
        return ('SET statement_timeout=' + str(milliseconds) + ';\n'
                '\\set native_auth_guard false\n' + guard + '\n\\gset\n'
                '\\if :native_auth_guard\n\\echo native_auth_guard_allowed\n' + selected +
                '\n\\else\n\\echo native_auth_guard_refused\n\\endif\n').encode('ascii')

    def __call__(self, step, query, seconds):
        if self._busy:
            self._clock_failed = True
            raise SqlBlocked('reentrant') from None
        self._busy = True
        try:
            return self._invoke(step, query, seconds)
        finally:
            self._busy = False

    def _invoke(self, step, query, seconds):
        try:
            finite = type(seconds) in (int, float) and math.isfinite(seconds)
        except (OverflowError, ValueError):
            finite = False
        if not finite or not 0.001 <= seconds <= 15:
            raise SqlBlocked('invalid_timeout') from None
        selected = self._selected_query(step, query)
        started = self._now()
        cutoff = started + seconds
        if type(cutoff) is float:
            cutoff = math.nextafter(cutoff, -math.inf)
        if not math.isfinite(cutoff) or cutoff <= started:
            raise SqlBlocked('invalid_timeout') from None
        # No credential-bearing argv/DSN or inherited environment. The only
        # password source is the explicitly selected protected passfile.
        c = self._config
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C', 'LC_ALL': 'C',
               'PGHOST': c['host'], 'PGPORT': str(c['port']), 'PGDATABASE': c['database'],
               'PGUSER': c['user'], 'PGPASSFILE': c['passfile'],
               'PGSSLMODE': 'verify-full', 'PGSSLROOTCERT': c['ca_file'],
               'PGGSSENCMODE': 'disable', 'PGSSLCERTMODE': 'disable',
               'PGCONNECT_TIMEOUT': str(max(1, math.floor(min(10, seconds)))),
               'PGAPPNAME': 'odbcpp-native-auth-cleanup'}
        try:
            verify_executable(Path(self._executable), self._sha256)
            validate_private_file(c['passfile'])
            remaining = cutoff - self._now()
            if type(remaining) is float:
                remaining = math.nextafter(remaining, -math.inf)
            if not 0.001 <= remaining <= seconds:
                return SqlReply(False)
            env['PGCONNECT_TIMEOUT'] = str(max(1, math.floor(min(10, remaining))))
            command = self._guarded_command(selected, math.floor(remaining * 1000))
            observed = subprocess.run([self._executable, '-X', '-w', '-qAt', '-v', 'ON_ERROR_STOP=1'],
                input=command, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                timeout=remaining, check=False, close_fds=True)
        except Exception:
            # Never expose TimeoutExpired.output/stderr, subprocess/config errors,
            # or the exception context containing private response diagnostics.
            return SqlReply(False)
        try:
            late = self._now() >= cutoff
        except Exception:
            return SqlReply(False)
        if (late or type(observed) is not subprocess.CompletedProcess or
                type(observed.returncode) is not int or observed.returncode != 0 or
                type(observed.stdout) is not bytes or len(observed.stdout) > 128 + len(_ALLOWED)):
            return SqlReply(False)
        if not observed.stdout.startswith(_ALLOWED):
            return SqlReply(False)
        payload = observed.stdout[len(_ALLOWED):]
        if len(payload) > 128:
            return SqlReply(False)
        try:
            text = payload.decode('ascii')
        except UnicodeError:
            return SqlReply(False)
        # Only fixed typed results may escape; no arbitrary server/error text.
        if step == 'cleanup_pids':
            records = text.removesuffix('\n').split('\n') if text else []
            if len(records) > 9 or any(not re.fullmatch(r'[1-9][0-9]{0,9}', pid, re.ASCII)
                                       or int(pid) > 2147483647 for pid in records):
                return SqlReply(False)
        elif not re.fullmatch(r'[tf]\n?', text, re.ASCII):
            return SqlReply(False)
        return SqlReply(True, text)
