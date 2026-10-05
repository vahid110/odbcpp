"""Explicit legacy declaration projection; no installation or authority."""
import os
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
import shutil
import tempfile

MEMBERS = ('core/database/backend_capabilities.h', 'core/database/backend_provider.h', 'core/database/backend_result.h', 'core/database/catalog_execution.h', 'core/database/catalog_queries.h', 'core/database/catalog_request.h', 'core/database/database_factory.h', 'core/database/error_policy.h', 'core/database/generic_database_connection.h', 'core/database/i_database_connection.h', 'core/database/i_protocol_parser.h', 'core/database/mysql/authentication.h', 'core/database/mysql/connection_security.h', 'core/database/mysql/date_parameter_descriptor.h', 'core/database/mysql/date_parameter_wire.h', 'core/database/mysql/date_wire.h', 'core/database/mysql/datetime_parameter_descriptor.h', 'core/database/mysql/datetime_parameter_wire.h', 'core/database/mysql/datetime_wire.h', 'core/database/mysql/decimal_wire.h', 'core/database/mysql/error_wire.h', 'core/database/mysql/handshake_wire.h', 'core/database/mysql/mysql_protocol_parser.h', 'core/database/mysql/mysql_session.h', 'core/database/mysql/parameter_receipt_shape.h', 'core/database/mysql/prepared_wire.h', 'core/database/mysql/query_wire.h', 'core/database/mysql/time_wire.h', 'core/database/mysql/tls_negotiation.h', 'core/database/native_type_info.h', 'core/database/native_type_resolution.h', 'core/database/parsed_query_result.h', 'core/database/postgres/pg_backend_provider.h', 'core/database/postgres/pg_catalog_profile.h', 'core/database/postgres/pg_command.h', 'core/database/postgres/pg_database_connection.h', 'core/database/postgres/pg_messages.h', 'core/database/postgres/pg_protocol_parser.h', 'core/database/postgres/pg_sql_dialect.h', 'core/database/postgres/pg_value.h', 'core/database/postgres/redshift_catalog_query.h', 'core/database/postgres/redshift_foreign_key_contract.h', 'core/database/postgres/redshift_primary_key_contract.h', 'core/database/postgres/scram_sha256.h', 'core/database/query_parameter.h', 'core/database/query_result.h', 'core/database/result_validation.h', 'core/database/session_health.h', 'core/database/session_reset.h', 'core/database/sql_dialect.h', 'core/database/sql_translation.h', 'core/database/statement_description.h', 'core/database/statement_kind.h', 'core/database/transaction.h', 'core/database/transaction_session.h', 'core/database/type_definition.h', 'core/transport/async_tls_transport.h', 'core/transport/async_transport.h', 'core/transport/deadline_model.h', 'core/transport/epoll_transport.h', 'core/transport/i_transport.h', 'core/transport/iocp_transport.h', 'core/transport/socket_transport.h', 'core/transport/socket_wait.h', 'core/transport/start_tls_transport.h', 'core/transport/thread_pool_transport.h', 'core/transport/tls_configurable_transport.h', 'core/transport/tls_transport.h', 'core/transport/transport_factory.h', 'core/transport/transport_options.h', 'core/util/base64.h', 'core/util/deadline.h', 'core/util/driver_logging.h', 'core/util/errors.h', 'core/util/exception_adapter.h', 'core/util/hex.h', 'core/util/platform.h', 'core/util/result.h', 'core/util/utf8.h')
OWNERS = {'core/auth/auth_core.h': 'sdk/internal/odbcpp/auth/auth_core.h', 'core/auth/aws_db_json_response.h': 'sdk/internal/odbcpp/auth/aws_db_json_response.h', 'core/auth/aws_db_response_fields.h': 'sdk/internal/odbcpp/auth/aws_db_response_fields.h', 'core/auth/aws_db_xml_response.h': 'sdk/internal/odbcpp/auth/aws_db_xml_response.h', 'core/auth/bounded_response_stream.h': 'sdk/internal/odbcpp/auth/bounded_response_stream.h', 'core/auth/checked_aws_db_json_response.h': 'sdk/internal/odbcpp/auth/checked_aws_db_json_response.h', 'core/auth/checked_aws_db_xml_response.h': 'sdk/internal/odbcpp/auth/checked_aws_db_xml_response.h', 'core/auth/checked_response_boundary.h': 'sdk/internal/odbcpp/auth/checked_response_boundary.h', 'core/auth/checked_response_stream.h': 'sdk/internal/odbcpp/auth/checked_response_stream.h', 'core/auth/detail/aws_db_json_parser.h': 'sdk/internal/odbcpp/auth/detail/aws_db_json_parser.h', 'core/auth/detail/aws_db_xml_parser.h': 'sdk/internal/odbcpp/auth/detail/aws_db_xml_parser.h', 'core/auth/issuer_timestamp.h': 'sdk/internal/odbcpp/auth/issuer_timestamp.h', 'core/auth/redshift_provisioned_response.h': 'sdk/internal/odbcpp/auth/redshift_provisioned_response.h', 'core/auth/redshift_serverless_response.h': 'sdk/internal/odbcpp/auth/redshift_serverless_response.h', 'core/auth/redshift_withiam_response.h': 'sdk/internal/odbcpp/auth/redshift_withiam_response.h', 'core/auth/response_operation.h': 'sdk/internal/odbcpp/auth/response_operation.h', 'core/auth/temporary_db_validity.h': 'sdk/internal/odbcpp/auth/temporary_db_validity.h', 'core/database/backend_capabilities.h': 'sdk/include/odbcpp/database/backend_capabilities.h', 'core/database/backend_provider.h': 'sdk/include/odbcpp/database/backend_provider.h', 'core/database/backend_result.h': 'sdk/include/odbcpp/database/backend_result.h', 'core/database/catalog_execution.h': 'sdk/include/odbcpp/database/catalog_execution.h', 'core/database/catalog_queries.h': 'sdk/include/odbcpp/database/catalog_queries.h', 'core/database/catalog_request.h': 'sdk/include/odbcpp/database/catalog_request.h', 'core/database/connection_pool.h': 'core/database/connection_pool.h', 'core/database/credential_context.h': 'sdk/internal/odbcpp/session/credential_context.h', 'core/database/database_factory.h': 'core/database/database_factory.h', 'core/database/error_policy.h': 'sdk/include/odbcpp/database/error_policy.h', 'core/database/generic_database_connection.h': 'core/database/generic_database_connection.h', 'core/database/i_database_connection.h': 'sdk/include/odbcpp/database/i_database_connection.h', 'core/database/i_protocol_parser.h': 'core/database/i_protocol_parser.h', 'core/database/mysql/authentication.h': 'core/database/mysql/authentication.h', 'core/database/mysql/connection_security.h': 'core/database/mysql/connection_security.h', 'core/database/mysql/date_parameter_descriptor.h': 'core/database/mysql/date_parameter_descriptor.h', 'core/database/mysql/date_parameter_wire.h': 'core/database/mysql/date_parameter_wire.h', 'core/database/mysql/date_wire.h': 'core/database/mysql/date_wire.h', 'core/database/mysql/datetime_parameter_descriptor.h': 'core/database/mysql/datetime_parameter_descriptor.h', 'core/database/mysql/datetime_parameter_wire.h': 'core/database/mysql/datetime_parameter_wire.h', 'core/database/mysql/datetime_wire.h': 'core/database/mysql/datetime_wire.h', 'core/database/mysql/decimal_wire.h': 'core/database/mysql/decimal_wire.h', 'core/database/mysql/error_wire.h': 'core/database/mysql/error_wire.h', 'core/database/mysql/handshake_wire.h': 'core/database/mysql/handshake_wire.h', 'core/database/mysql/mysql_protocol_parser.h': 'core/database/mysql/mysql_protocol_parser.h', 'core/database/mysql/mysql_session.h': 'core/database/mysql/mysql_session.h', 'core/database/mysql/parameter_receipt_shape.h': 'core/database/mysql/parameter_receipt_shape.h', 'core/database/mysql/prepared_wire.h': 'core/database/mysql/prepared_wire.h', 'core/database/mysql/query_wire.h': 'core/database/mysql/query_wire.h', 'core/database/mysql/time_wire.h': 'core/database/mysql/time_wire.h', 'core/database/mysql/tls_negotiation.h': 'core/database/mysql/tls_negotiation.h', 'core/database/native_type_info.h': 'sdk/include/odbcpp/database/native_type_info.h', 'core/database/native_type_resolution.h': 'core/database/native_type_resolution.h', 'core/database/parsed_query_result.h': 'core/database/parsed_query_result.h', 'core/database/postgres/pg_backend_provider.h': 'core/database/postgres/pg_backend_provider.h', 'core/database/postgres/pg_catalog_profile.h': 'core/database/postgres/pg_catalog_profile.h', 'core/database/postgres/pg_command.h': 'core/database/postgres/pg_command.h', 'core/database/postgres/pg_database_connection.h': 'core/database/postgres/pg_database_connection.h', 'core/database/postgres/pg_messages.h': 'core/database/postgres/pg_messages.h', 'core/database/postgres/pg_protocol_parser.h': 'core/database/postgres/pg_protocol_parser.h', 'core/database/postgres/pg_sql_dialect.h': 'core/database/postgres/pg_sql_dialect.h', 'core/database/postgres/pg_value.h': 'core/database/postgres/pg_value.h', 'core/database/postgres/redshift_catalog_query.h': 'core/database/postgres/redshift_catalog_query.h', 'core/database/postgres/redshift_foreign_key_contract.h': 'core/database/postgres/redshift_foreign_key_contract.h', 'core/database/postgres/redshift_primary_key_contract.h': 'core/database/postgres/redshift_primary_key_contract.h', 'core/database/postgres/scram_sha256.h': 'core/database/postgres/scram_sha256.h', 'core/database/query_parameter.h': 'sdk/include/odbcpp/database/query_parameter.h', 'core/database/query_result.h': 'sdk/include/odbcpp/database/query_result.h', 'core/database/result_validation.h': 'core/database/result_validation.h', 'core/database/session_health.h': 'sdk/include/odbcpp/database/session_health.h', 'core/database/session_owner.h': 'sdk/internal/odbcpp/session/session_owner.h', 'core/database/session_reset.h': 'sdk/include/odbcpp/database/session_reset.h', 'core/database/sql_dialect.h': 'sdk/include/odbcpp/database/sql_dialect.h', 'core/database/sql_translation.h': 'sdk/include/odbcpp/database/sql_translation.h', 'core/database/statement_description.h': 'sdk/include/odbcpp/database/statement_description.h', 'core/database/statement_kind.h': 'sdk/include/odbcpp/database/statement_kind.h', 'core/database/transaction.h': 'sdk/include/odbcpp/database/transaction.h', 'core/database/transaction_session.h': 'sdk/include/odbcpp/database/transaction_session.h', 'core/database/type_definition.h': 'sdk/include/odbcpp/database/type_definition.h', 'core/security/crypto.h': 'sdk/internal/odbcpp/security/crypto.h', 'core/security/tls_client.h': 'sdk/internal/odbcpp/security/tls_client.h', 'core/transport/async_tls_transport.h': 'sdk/internal/odbcpp/transport/async_tls_transport.h', 'core/transport/async_transport.h': 'sdk/internal/odbcpp/transport/async_transport.h', 'core/transport/deadline_model.h': 'sdk/internal/odbcpp/transport/deadline_model.h', 'core/transport/epoll_transport.h': 'sdk/internal/odbcpp/transport/epoll_transport.h', 'core/transport/i_transport.h': 'sdk/include/odbcpp/transport/i_transport.h', 'core/transport/iocp_transport.h': 'sdk/internal/odbcpp/transport/iocp_transport.h', 'core/transport/socket_transport.h': 'sdk/internal/odbcpp/transport/socket_transport.h', 'core/transport/socket_wait.h': 'sdk/internal/odbcpp/transport/socket_wait.h', 'core/transport/start_tls_transport.h': 'sdk/internal/odbcpp/transport/start_tls_transport.h', 'core/transport/thread_pool_transport.h': 'sdk/internal/odbcpp/transport/thread_pool_transport.h', 'core/transport/tls_configurable_transport.h': 'sdk/internal/odbcpp/transport/tls_configurable_transport.h', 'core/transport/tls_transport.h': 'sdk/internal/odbcpp/transport/tls_transport.h', 'core/transport/transport_factory.h': 'sdk/internal/odbcpp/transport/transport_factory.h', 'core/transport/transport_options.h': 'sdk/internal/odbcpp/transport/transport_options.h', 'core/util/base64.h': 'sdk/internal/odbcpp/util/base64.h', 'core/util/deadline.h': 'sdk/include/odbcpp/util/deadline.h', 'core/util/driver_logging.h': 'sdk/internal/odbcpp/util/driver_logging.h', 'core/util/errors.h': 'sdk/internal/odbcpp/util/errors.h', 'core/util/exception_adapter.h': 'sdk/internal/odbcpp/util/exception_adapter.h', 'core/util/hex.h': 'sdk/internal/odbcpp/util/hex.h', 'core/util/platform.h': 'sdk/internal/odbcpp/util/platform.h', 'core/util/result.h': 'sdk/include/odbcpp/util/result.h', 'core/util/utf8.h': 'sdk/internal/odbcpp/util/utf8.h'}
MAX_HEADER = 1024 * 1024
MAX_TOTAL = 16 * 1024 * 1024
_MARKER = b'PRIVATE_LEGACY_HEADER_PROJECTION_V1\n'

class ProjectionError(ValueError):
    pass

def _path(value):
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9_./-]+', value):
        raise ProjectionError('Invalid path')
    parts = PurePosixPath(value).parts
    if any(not x for x in value.split('/')) or PurePosixPath(value).as_posix() != value:
        raise ProjectionError('Noncanonical path')
    if value.startswith('/') or any(x in ('.', '..') for x in value.split('/')) or not parts:
        raise ProjectionError('Escaping path')
    return value

def _visible(text):
    # Mask comments and literals, including raw literals, while retaining offsets.
    # Closed supported grammar: no translation-phase line splices.
    if '\\\n' in text or '\\\r\n' in text:
        raise ProjectionError('Unsupported line splice')
    chars = list(text); i = 0
    while i < len(text):
        end = None
        if text.startswith('//', i):
            end = text.find('\n', i)
            if end < 0: end = len(text)
        elif text.startswith('/*', i):
            close = text.find('*/', i + 2)
            if close < 0: raise ProjectionError('Unterminated comment')
            end = close + 2
        elif text.startswith('R"', i):
            match = re.match(r'R"([^ ()\\\t\r\n]{0,16})\(', text[i:])
            if match:
                close = text.find(')' + match[1] + '"', i + len(match[0]))
                if close < 0: raise ProjectionError('Unterminated raw literal')
                end = close + len(match[1]) + 2
        elif text[i] in ('"', "'"):
            quote = text[i]; j = i + 1
            while j < len(text):
                if text[j] == '\\': j += 2; continue
                if text[j] == quote: break
                if text[j] == '\n': raise ProjectionError('Unterminated literal')
                j += 1
            if j >= len(text): raise ProjectionError('Unterminated literal')
            end = j + 1
        if end is not None:
            for j in range(i, end):
                if chars[j] not in '\r\n': chars[j] = ' '
            i = end
        else: i += 1
    return ''.join(chars)

def project(sources, owners=None):
    selected = dict(OWNERS if owners is None else owners)
    if set(selected) != set(OWNERS) or set(MEMBERS) - set(selected):
        raise ProjectionError('Owner membership mismatch')
    for old, owner in selected.items(): _path(old); _path(owner)
    if len(set(selected.values())) != len(selected):
        raise ProjectionError('Ambiguous owner')
    for old, owner in selected.items(): _path(old); _path(owner)
    if set(sources) != set(selected.values()): raise ProjectionError('Missing or extra owner')
    if any(type(x) is not bytes or len(x) > MAX_HEADER for x in sources.values()):
        raise ProjectionError('Invalid header bytes')
    if sum(len(x) for x in sources.values()) > MAX_TOTAL: raise ProjectionError('Aggregate limit')
    reverse = {}
    for old, owner in selected.items():
        logical = owner
        for prefix in ('sdk/include/', 'sdk/internal/'):
            if owner.startswith(prefix): logical = owner[len(prefix):]; break
        if logical in reverse: raise ProjectionError('Ambiguous include')
        reverse[logical] = old
    by_physical = {owner: old for old, owner in selected.items()}
    result = {}
    for old in MEMBERS:
        owner = selected[old]
        try: text = sources[owner].decode('utf-8')
        except UnicodeError as exc: raise ProjectionError('Invalid header encoding') from exc
        visible = _visible(text); edits = []
        if '\v' in visible or '\f' in visible:
            raise ProjectionError('Unsupported preprocessing whitespace')
        for directive in re.finditer(r'(?m)^[ \t]*#[ \t]*include\b[^\n]*', visible):
            raw = text[directive.start():directive.end()]
            # The match excludes LF but includes its CR in a CRLF checkout.
            # Remove only that terminator for grammar matching; preserve source bytes.
            if raw.endswith('\r') and text[directive.end():directive.end() + 1] == '\n':
                raw = raw[:-1]
            match = re.fullmatch(r'[ \t]*#[ \t]*include[ \t]+([<"])([^>"\r\n]+)([>"])([ \t]*(?://[^\n]*)?)', raw)
            if not match or (match[1], match[3]) not in (('<', '>'), ('"', '"')):
                raise ProjectionError('Unverifiable include')
            token = match[2]
            if token.startswith(('odbcpp/', 'core/', 'odbc/', 'product/')):
                _path(token)
                if token not in reverse: raise ProjectionError('Unknown project include')
                replacement = reverse[token]
            elif match[1] == '"':
                _path(token)
                physical = str(PurePosixPath(owner).parent / token)
                if physical not in by_physical: raise ProjectionError('Unresolved relative include')
                replacement = by_physical[physical]
            else:
                if '\\' in token or '..' in token.split('/') or token.startswith('/'):
                    raise ProjectionError('Invalid standard include')
                continue
            start = directive.start() + match.start(2);end = directive.start() + match.end(2)
            edits.append((start, end, replacement))
        for start, end, replacement in reversed(edits): text = text[:start] + replacement + text[end:]
        result[old] = text.encode('utf-8')
    return result

@dataclass(frozen=True)
class PublicationResult:
    published: bool
    cleanup_complete: bool
    cleanup_uncertain: bool

class PublicationFailure(OSError):
    def __init__(self, prior_restored):
        super().__init__('Publication failed')
        self.published = False
        self.prior_restored = prior_restored
        self.cleanup_uncertain = not prior_restored

def restage(output, projected):
    """Private publication: explicit rollback versus postcommit cleanup outcome."""
    output = Path(output)
    if '..' in output.parts:
        raise ProjectionError('Escaping output')
    if set(projected) != set(MEMBERS) or any(type(v) is not bytes for v in projected.values()):
        raise ProjectionError('Projection membership mismatch')
    if any(len(x) > MAX_HEADER for x in projected.values()) or sum(len(x) for x in projected.values()) > MAX_TOTAL:
        raise ProjectionError('Output byte limit')
    if output.is_symlink() or not output.parent.is_dir() or output.parent.is_symlink():
        raise ProjectionError('Invalid output owner')
    if output.exists() and (not output.is_dir() or (output / '.projection-owner').is_symlink() or not (output / '.projection-owner').is_file() or (output / '.projection-owner').read_bytes() != _MARKER):
        raise ProjectionError('Unowned output collision')
    temporary = Path(tempfile.mkdtemp(prefix='.projection-', dir=str(output.parent)))
    backup = output.parent / (temporary.name + '-old')
    moved_old = False
    try:
        for path in MEMBERS:
            target = temporary / path;target.parent.mkdir(parents=True, exist_ok=True);target.write_bytes(projected[path])
        (temporary / '.projection-owner').write_bytes(_MARKER)
        if output.exists():
            os.replace(str(output), str(backup));moved_old = True
        try:
            os.replace(str(temporary), str(output))
        except OSError as failure:
            restored = not moved_old
            if moved_old:
                try:
                    os.replace(str(backup), str(output));restored = True
                except OSError:
                    restored = False
            raise PublicationFailure(restored) from failure
    except BaseException as failure:
        # Cleanup is best effort; retain rollback backup if restoration failed.
        if temporary.exists():
            try: shutil.rmtree(temporary)
            except OSError:
                if isinstance(failure, OSError): failure.cleanup_uncertain = True
        if isinstance(failure, OSError) and not isinstance(failure, PublicationFailure):
            failure.published = False
            failure.prior_restored = not moved_old
            if not hasattr(failure, 'cleanup_uncertain'): failure.cleanup_uncertain = False
        raise
    # Commit completed. Never roll back a possibly partially removed backup.
    if backup.exists():
        try: shutil.rmtree(backup)
        except OSError:
            return PublicationResult(True, False, True)
    return PublicationResult(True, True, False)
