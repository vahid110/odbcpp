"""Fixed externally supplied IAM DB credential exchange for the reserved launcher.

No SQL, renewal or admission here. The reviewed launcher must reserve first,
keep credential values private, and include this principal in independent cleanup.
This is not a native driver IAM provider.
"""
from dataclasses import dataclass, field
from datetime import datetime, timezone

from tools.redshift import pilot_live as live
from tools.redshift.pilot_preflight import Blocked

ROLE_ARN = 'arn:aws:iam::385207570137:role/odbcpp-redshift-test/odbcpp-redshift-test-runtime'
ROLE_NAME = 'odbcpp-redshift-test-runtime'
SESSION_NAME = 'odbcpp-redshift-qualification'
DB_USER = live.IAM_DB_USER
DATABASE = 'odbcpp_pilot'


def need(condition, reason):
    if not condition:
        raise Blocked(reason)


def timestamp(value):
    try:
        need(isinstance(value, str) and len(value) <= 64, 'iam_expiration_invalid')
        parsed = datetime.fromisoformat(value.replace('Z', '+00:00'))
        need(parsed.tzinfo is not None and parsed.utcoffset() == timezone.utc.utcoffset(parsed),
             'iam_expiration_invalid')
        return parsed
    except (ValueError, TypeError):
        raise Blocked('iam_expiration_invalid') from None


def validity(expiration, now, execution_end):
    need(isinstance(now, datetime) and isinstance(execution_end, datetime)
         and now.tzinfo is not None and execution_end.tzinfo is not None,
         'iam_clock_invalid')
    remaining = (execution_end - now).total_seconds()
    need(0 < remaining <= 180, 'iam_execution_bound_invalid')
    expires = timestamp(expiration)
    # Requests will specify900 seconds. Never refresh or shorten cleanup margin.
    need(remaining + 60 <= (expires - now).total_seconds() <= 960,
         'iam_credential_validity_insufficient')
    return expires


def secret(value, minimum, maximum):
    need(isinstance(value, str) and minimum <= len(value) <= maximum
         and all(32 <= ord(c) <= 126 for c in value), 'iam_credential_invalid')
    return value


@dataclass(frozen=True, repr=False)
class RoleCredentials:
    access_key: str = field(repr=False)
    secret_key: str = field(repr=False)
    token: str = field(repr=False)
    expires: datetime

    def __repr__(self):
        return 'RoleCredentials(<redacted>)'

    def environment(self):
        env = live.clean_env()
        env.update(AWS_ACCESS_KEY_ID=self.access_key, AWS_SECRET_ACCESS_KEY=self.secret_key,
                   AWS_SESSION_TOKEN=self.token, AWS_PAGER='', AWS_MAX_ATTEMPTS='1',
                   AWS_CONFIG_FILE='/dev/null', AWS_SHARED_CREDENTIALS_FILE='/dev/null')
        return env


@dataclass(frozen=True, repr=False)
class DatabaseCredentials:
    user: str
    password: str = field(repr=False)
    expires: datetime

    def __repr__(self):
        return 'DatabaseCredentials(<redacted>)'


def role_credentials(response, now, execution_end):
    need(isinstance(response, dict), 'iam_role_response_invalid')
    identity = response.get('AssumedRoleUser')
    expected = f'arn:aws:sts::{live.ACCOUNT}:assumed-role/{ROLE_NAME}/{SESSION_NAME}'
    need(isinstance(identity, dict) and identity.get('Arn') == expected,
         'iam_role_identity_mismatch')
    c = response.get('Credentials')
    need(isinstance(c, dict), 'iam_role_response_invalid')
    expires = validity(c.get('Expiration'), now, execution_end)
    return RoleCredentials(secret(c.get('AccessKeyId'), 16, 128),
                           secret(c.get('SecretAccessKey'), 20, 256),
                           secret(c.get('SessionToken'), 20, 16384), expires)


def verify_role_identity(identity):
    expected = f'arn:aws:sts::{live.ACCOUNT}:assumed-role/{ROLE_NAME}/{SESSION_NAME}'
    need(isinstance(identity, dict) and identity.get('Account') == live.ACCOUNT
         and identity.get('Arn') == expected, 'iam_role_identity_mismatch')


def database_credentials(response, now, execution_end):
    need(isinstance(response, dict) and set(response) <=
         {'dbUser', 'dbPassword', 'expiration', 'nextRefreshTime'}, 'iam_db_response_invalid')
    need(response.get('dbUser') == DB_USER, 'iam_db_principal_mismatch')
    expires = validity(response.get('expiration'), now, execution_end)
    if 'nextRefreshTime' in response:
        # Record no refresh promise, but reject malformed provenance.
        timestamp(response['nextRefreshTime'])
    return DatabaseCredentials(DB_USER, secret(response.get('dbPassword'), 20, 256), expires)


def brace(value):
    need(isinstance(value, str) and value and not any(ord(c) < 32 or ord(c) == 127 for c in value),
         'iam_connection_value_invalid')
    return '{' + value.replace('}', '}}') + '}'


def connection_string(config, credentials, now, execution_end):
    live.validate_config(config)
    need(isinstance(credentials, DatabaseCredentials) and credentials.user == DB_USER,
         'iam_db_principal_mismatch')
    validity(credentials.expires.isoformat(), now, execution_end)
    return (f"SERVER={config['host']};PORT=5439;DATABASE={DATABASE};"
            f"UID={brace(credentials.user)};PWD={brace(secret(credentials.password,20,256))};"
            f"SSL=1;SSLCAFILE={config['ca_file']};")


def credential_request():
    # Exact request inventory for the future reserved launcher, not executable here.
    return ['redshift-serverless', 'get-credentials', '--workgroup-name', live.NAME,
            '--db-name', DATABASE, '--duration-seconds', '900']


def acquire(execution_end, deadline):
    """One fixed credential exchange, called only after durable window reservation."""
    import json
    import subprocess
    import time
    from tools.redshift.pilot_preflight import _unique_object

    def call(args, env):
        seconds = min(20, int(deadline - time.monotonic()))
        need(seconds >= 1, 'execution_deadline')
        try:
            result = subprocess.run([live.AWS, '--region', live.REGION, '--no-cli-pager'] + args,
                                    env=env, capture_output=True, timeout=seconds)
            need(result.returncode == 0 and len(result.stdout) <= 65536,
                 'iam_exchange_failed')
            return json.loads(result.stdout, object_pairs_hook=_unique_object,
                parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
        except (OSError, ValueError, subprocess.SubprocessError):
            raise Blocked('iam_exchange_failed') from None

    env = live.clean_env(); env.update(AWS_PAGER='', AWS_MAX_ATTEMPTS='1')
    role = role_credentials(call(['--profile',live.NAME,'sts','assume-role',
        '--role-arn',ROLE_ARN,'--role-session-name',SESSION_NAME,'--duration-seconds','900'],env),
        datetime.now(timezone.utc),execution_end)
    role_env = role.environment()
    validity(role.expires.isoformat(),datetime.now(timezone.utc),execution_end)
    verify_role_identity(call(['sts','get-caller-identity'],role_env))
    validity(role.expires.isoformat(),datetime.now(timezone.utc),execution_end)
    response = call(credential_request(),role_env)
    return database_credentials(response,datetime.now(timezone.utc),execution_end)
