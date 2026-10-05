import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('projection', Path(__file__).resolve().parents[2] / 'tools/ci/sdk_legacy_header_projection.py')
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)

Q6_ADDITIONS = ('core/database/mysql/datetime_parameter_descriptor.h', 'core/database/mysql/datetime_parameter_wire.h')
ORIGINAL_MEMBERS = ('core/database/backend_capabilities.h', 'core/database/backend_provider.h', 'core/database/backend_result.h', 'core/database/catalog_execution.h', 'core/database/catalog_queries.h', 'core/database/catalog_request.h', 'core/database/database_factory.h', 'core/database/error_policy.h', 'core/database/generic_database_connection.h', 'core/database/i_database_connection.h', 'core/database/i_protocol_parser.h', 'core/database/mysql/authentication.h', 'core/database/mysql/connection_security.h', 'core/database/mysql/date_parameter_descriptor.h', 'core/database/mysql/date_parameter_wire.h', 'core/database/mysql/date_wire.h', 'core/database/mysql/datetime_wire.h', 'core/database/mysql/decimal_wire.h', 'core/database/mysql/error_wire.h', 'core/database/mysql/handshake_wire.h', 'core/database/mysql/mysql_protocol_parser.h', 'core/database/mysql/mysql_session.h', 'core/database/mysql/parameter_receipt_shape.h', 'core/database/mysql/prepared_wire.h', 'core/database/mysql/query_wire.h', 'core/database/mysql/time_wire.h', 'core/database/mysql/tls_negotiation.h', 'core/database/native_type_info.h', 'core/database/native_type_resolution.h', 'core/database/parsed_query_result.h', 'core/database/postgres/pg_backend_provider.h', 'core/database/postgres/pg_catalog_profile.h', 'core/database/postgres/pg_command.h', 'core/database/postgres/pg_database_connection.h', 'core/database/postgres/pg_messages.h', 'core/database/postgres/pg_protocol_parser.h', 'core/database/postgres/pg_sql_dialect.h', 'core/database/postgres/pg_value.h', 'core/database/postgres/redshift_catalog_query.h', 'core/database/postgres/redshift_foreign_key_contract.h', 'core/database/postgres/redshift_primary_key_contract.h', 'core/database/postgres/scram_sha256.h', 'core/database/query_parameter.h', 'core/database/query_result.h', 'core/database/result_validation.h', 'core/database/session_health.h', 'core/database/session_reset.h', 'core/database/sql_dialect.h', 'core/database/sql_translation.h', 'core/database/statement_description.h', 'core/database/statement_kind.h', 'core/database/transaction.h', 'core/database/transaction_session.h', 'core/database/type_definition.h', 'core/transport/async_tls_transport.h', 'core/transport/async_transport.h', 'core/transport/deadline_model.h', 'core/transport/epoll_transport.h', 'core/transport/i_transport.h', 'core/transport/iocp_transport.h', 'core/transport/socket_transport.h', 'core/transport/socket_wait.h', 'core/transport/start_tls_transport.h', 'core/transport/thread_pool_transport.h', 'core/transport/tls_configurable_transport.h', 'core/transport/tls_transport.h', 'core/transport/transport_factory.h', 'core/transport/transport_options.h', 'core/util/base64.h', 'core/util/deadline.h', 'core/util/driver_logging.h', 'core/util/errors.h', 'core/util/exception_adapter.h', 'core/util/hex.h', 'core/util/platform.h', 'core/util/result.h', 'core/util/utf8.h')
ORIGINAL_EXCLUSIONS = ('core/auth/auth_core.h', 'core/auth/aws_db_json_response.h', 'core/auth/aws_db_response_fields.h', 'core/auth/aws_db_xml_response.h', 'core/auth/bounded_response_stream.h', 'core/auth/checked_aws_db_json_response.h', 'core/auth/checked_aws_db_xml_response.h', 'core/auth/checked_response_boundary.h', 'core/auth/checked_response_stream.h', 'core/auth/detail/aws_db_json_parser.h', 'core/auth/detail/aws_db_xml_parser.h', 'core/auth/issuer_timestamp.h', 'core/auth/redshift_provisioned_response.h', 'core/auth/redshift_serverless_response.h', 'core/auth/redshift_withiam_response.h', 'core/auth/response_operation.h', 'core/auth/temporary_db_validity.h', 'core/database/connection_pool.h', 'core/database/credential_context.h', 'core/database/session_owner.h', 'core/security/crypto.h', 'core/security/tls_client.h')

class ProjectionTests(unittest.TestCase):
    def sources(self):
        return {x: b'#pragma once\n' for x in p.OWNERS.values()}

    def test_exact_79_members_original_77_and_22_exclusions_and_missing_crypto_edge(self):
        sources = self.sources()
        sources[p.OWNERS['core/database/mysql/connection_security.h']] = b'#include "odbcpp/security/crypto.h"\n'
        result = p.project(sources)
        self.assertEqual(len(result), 79)
        self.assertEqual(len(set(result) - set(Q6_ADDITIONS)), 77)
        self.assertEqual(len(set(p.OWNERS) - set(result)), 22)
        self.assertNotIn('core/security/crypto.h', result)
        self.assertEqual(result['core/database/mysql/connection_security.h'], b'#include "core/security/crypto.h"\n')

    def test_only_tokens_changed_comments_strings_standard_raw_untouched(self):
        source = b'#include <array>\n#include "odbcpp/util/deadline.h" // note\n// #include "odbcpp/evil.h"\nconst char* s="odbcpp/util/deadline.h";\nconst char* r=R"tag(\n#include "odbcpp/evil.h"\n)tag";\n'
        sources = self.sources();key = p.OWNERS['core/database/query_parameter.h'];sources[key] = source
        expected = source.replace(b'#include "odbcpp/util/deadline.h"', b'#include "core/util/deadline.h"', 1)
        self.assertEqual(p.project(sources)['core/database/query_parameter.h'], expected)

    def test_relative_dependency_has_unique_owner(self):
        sources = self.sources();key = p.OWNERS['core/database/query_result.h'];sources[key] = b'#include "native_type_info.h"\n'
        self.assertEqual(p.project(sources)['core/database/query_result.h'], b'#include "core/database/native_type_info.h"\n')

    def test_unknown_macro_unresolved_and_hostile_includes_refused(self):
        for value in (b'#include "odbcpp/unknown.h"\n', b'#include HEADER\n', b'#include "missing.h"\n', b'#include "../escape.h"\n', b'#include <core/unknown.h>\n', b'#include <../array>\n'):
            with self.subTest(value=value):
                sources=self.sources();sources[p.OWNERS[p.MEMBERS[0]]]=value
                with self.assertRaises(p.ProjectionError):p.project(sources)

    def test_missing_extra_and_duplicate_owners_refused(self):
        sources=self.sources();sources.pop(next(iter(sources)))
        with self.assertRaises(p.ProjectionError):p.project(sources)
        sources=self.sources();sources['extra.h']=b''
        with self.assertRaises(p.ProjectionError):p.project(sources)
        owners=dict(p.OWNERS);owners[p.MEMBERS[1]]=owners[p.MEMBERS[0]]
        with self.assertRaises(p.ProjectionError):p.project(self.sources(),owners)

    def test_escaping_owner_and_invalid_bytes_bounds(self):
        owners=dict(p.OWNERS);owners[p.MEMBERS[0]]='../bad.h'
        with self.assertRaises(p.ProjectionError):p.project(self.sources(),owners)
        for value in (bytearray(b'x'), b'\xff', b'x'*(p.MAX_HEADER+1)):
            sources=self.sources();sources[p.OWNERS[p.MEMBERS[0]]]=value
            with self.assertRaises(p.ProjectionError):p.project(sources)

    def test_aggregate_bound_no_output(self):
        sources={x:b'x'*200000 for x in p.OWNERS.values()}
        with self.assertRaises(p.ProjectionError):p.project(sources)

    def test_clean_restaging_and_spaces(self):
        with tempfile.TemporaryDirectory() as name:
            out=Path(name)/'prefix with spaces';values=p.project(self.sources());p.restage(out,values)
            (out/'stale.h').write_text('stale');p.restage(out,values)
            self.assertFalse((out/'stale.h').exists())
            self.assertEqual({x.relative_to(out).as_posix() for x in out.rglob('*.h')},set(p.MEMBERS))

    def test_unowned_output_symlink_and_partial_membership_refused(self):
        with tempfile.TemporaryDirectory() as name:
            out=Path(name)/'out';out.mkdir();(out/'keep').write_text('keep')
            with self.assertRaises(p.ProjectionError):p.restage(out,p.project(self.sources()))
            self.assertEqual((out/'keep').read_text(),'keep')
            link=Path(name)/'link';link.symlink_to(out,target_is_directory=True)
            with self.assertRaises(p.ProjectionError):p.restage(link,p.project(self.sources()))
            with self.assertRaises(p.ProjectionError):p.restage(Path(name)/'fresh',{})
            self.assertFalse((Path(name)/'fresh').exists())

    def test_write_failure_preserves_previous_tree(self):
        with tempfile.TemporaryDirectory() as name:
            out=Path(name)/'out';values=p.project(self.sources());p.restage(out,values)
            before={x.relative_to(out).as_posix():x.read_bytes() for x in out.rglob('*') if x.is_file()}
            with mock.patch.object(Path,'write_bytes',side_effect=OSError('injected')):
                with self.assertRaises(OSError):p.restage(out,values)
            self.assertEqual(before,{x.relative_to(out).as_posix():x.read_bytes() for x in out.rglob('*') if x.is_file()})
            self.assertFalse(list(Path(name).glob('.projection-*')))

    def test_publish_replace_failure_rolls_back(self):
        with tempfile.TemporaryDirectory() as name:
            out=Path(name)/'out';values=p.project(self.sources());p.restage(out,values)
            original=p.os.replace;calls=[]
            def replace(a,b):
                calls.append((a,b))
                if len(calls)==2:raise OSError('publish failed')
                return original(a,b)
            with mock.patch.object(p.os,'replace',side_effect=replace):
                with self.assertRaises(OSError):p.restage(out,values)
            self.assertTrue((out/'.projection-owner').is_file())
            self.assertEqual((out/p.MEMBERS[0]).read_bytes(),values[p.MEMBERS[0]])
            self.assertFalse(list(Path(name).glob('.projection-*')))

    def test_translation_phase_splice_closed_refusal_without_source_mutation(self):
        for source in (b'// continued '+bytes([92,10])+b'#include "odbcpp/util/deadline.h"\n',
                       b'#inc'+bytes([92,10])+b'lude "odbcpp/unknown.h"\n',
                       b'#inc'+bytes([92,13,10])+b'lude <array>\n'):
            with self.subTest(source=source):
                sources=self.sources();key=p.OWNERS[p.MEMBERS[0]];sources[key]=source
                with self.assertRaisesRegex(p.ProjectionError,'line splice'):p.project(sources)
                self.assertEqual(sources[key],source)

    def test_vt_ff_directive_refusal(self):
        for source in (b'\v#include "odbcpp/unknown.h"\n',b'#\finclude "odbcpp/unknown.h"\n',b'#include\v<array>\n'):
            sources=self.sources();sources[p.OWNERS[p.MEMBERS[0]]]=source
            with self.assertRaisesRegex(p.ProjectionError,'whitespace'):p.project(sources)

    def test_noncanonical_physical_aliases_before_source_graph(self):
        for alias in ('sdk//include/odbcpp/database/backend_capabilities.h',
                      'sdk/include/odbcpp/database/backend_capabilities.h/',
                      'sdk/include/./odbcpp/database/backend_capabilities.h'):
            owners=dict(p.OWNERS);owners[p.MEMBERS[1]]=alias
            with self.assertRaisesRegex(p.ProjectionError,'path'):p.project(self.sources(),owners)
        self.assertEqual(set(p.project(self.sources())),set(p.MEMBERS))

    def test_postcommit_partial_backup_cleanup_reports_new_publication(self):
        with tempfile.TemporaryDirectory() as name:
            out=Path(name)/'out';old=p.project(self.sources());p.restage(out,old)
            new=dict(old);new[p.MEMBERS[0]]=b'new bytes\n';original=p.shutil.rmtree
            def remove(path):
                path=Path(path)
                if path.name.endswith('-old'):
                    (path/p.MEMBERS[0]).unlink()
                    raise OSError('partial cleanup')
                return original(path)
            with mock.patch.object(p.shutil,'rmtree',side_effect=remove):result=p.restage(out,new)
            self.assertTrue(result.published);self.assertFalse(result.cleanup_complete);self.assertTrue(result.cleanup_uncertain)
            self.assertEqual((out/p.MEMBERS[0]).read_bytes(),new[p.MEMBERS[0]])
            backups=list(Path(name).glob('.projection-*-old'));self.assertEqual(len(backups),1)
            self.assertFalse((backups[0]/p.MEMBERS[0]).exists())

    def test_failed_publish_failed_rollback_retains_old_backup_uncertainty(self):
        with tempfile.TemporaryDirectory() as name:
            out=Path(name)/'out';old=p.project(self.sources());p.restage(out,old)
            original=p.os.replace;calls=[]
            def replace(a,b):
                calls.append((a,b))
                if len(calls)>1:raise OSError('publish and rollback fail')
                return original(a,b)
            with mock.patch.object(p.os,'replace',side_effect=replace):
                with self.assertRaises(p.PublicationFailure) as caught:p.restage(out,old)
            self.assertFalse(caught.exception.published);self.assertFalse(caught.exception.prior_restored);self.assertTrue(caught.exception.cleanup_uncertain)
            self.assertFalse(out.exists())
            backups=list(Path(name).glob('.projection-*-old'));self.assertEqual(len(backups),1)
            self.assertEqual((backups[0]/p.MEMBERS[0]).read_bytes(),old[p.MEMBERS[0]])

class ProjectionLineEndingTests(unittest.TestCase):
    def sources(self):
        return {x: b'#pragma once\n' for x in p.OWNERS.values()}
    # Only these regressions are added; inherited tests stay in their original class.
    def test_crlf_and_mixed_line_endings_preserve_every_non_token_byte(self):
        source = b'#include <array>\r\n#include "odbcpp/util/deadline.h" // note\r\n#include "native_type_info.h"\n// comment\r\n'
        sources = self.sources();sources[p.OWNERS['core/database/query_result.h']] = source
        expected = source.replace(b'odbcpp/util/deadline.h', b'core/util/deadline.h').replace(b'"native_type_info.h"', b'"core/database/native_type_info.h"')
        self.assertEqual(p.project(sources)['core/database/query_result.h'], expected)
        self.assertEqual(sources[p.OWNERS['core/database/query_result.h']], source)

    def test_cr_inside_token_or_directive_and_bare_terminal_cr_refused(self):
        for source in (b'#include "odbcpp/util/dead\rline.h"\r\n',
                       b'#include\r "odbcpp/util/deadline.h"\r\n',
                       b'#include <array>\r', b'#include <array>\r\r\n'):
            with self.subTest(source=source):
                sources=self.sources();sources[p.OWNERS[p.MEMBERS[0]]]=source
                with self.assertRaises(p.ProjectionError):p.project(sources)

    def test_crlf_does_not_relax_macro_unknown_or_spliced_include_refusal(self):
        for source in (b'#include HEADER\r\n', b'#include "odbcpp/unknown.h"\r\n',
                       b'#inc'+bytes([92,13,10])+b'lude <array>\r\n'):
            with self.subTest(source=source):
                sources=self.sources();sources[p.OWNERS[p.MEMBERS[0]]]=source
                with self.assertRaises(p.ProjectionError):p.project(sources)


class ProjectionQ6MembershipTests(unittest.TestCase):
    def sources(self):
        return {owner: b'#pragma once\n' for owner in p.OWNERS.values()}

    def test_exact_selected_additions_and_original_subsets(self):
        result = p.project(self.sources())
        self.assertEqual(len(p.OWNERS), 101)
        self.assertEqual(tuple(sorted(result)), tuple(sorted(ORIGINAL_MEMBERS + Q6_ADDITIONS)))
        self.assertEqual(tuple(member for member in p.MEMBERS if member not in Q6_ADDITIONS), ORIGINAL_MEMBERS)
        self.assertEqual(tuple(sorted(set(p.OWNERS) - set(result))), ORIGINAL_EXCLUSIONS)
        self.assertEqual(len(ORIGINAL_EXCLUSIONS), 22)
        for member in Q6_ADDITIONS:
            self.assertEqual(p.OWNERS[member], member)
        self.assertNotIn('core/security/crypto.h', result)

    def test_manifest_and_independent_consumer_inventory_agree(self):
        root = Path(__file__).resolve().parents[2]
        self.assertEqual((root / 'sdk/legacy_install_headers.txt').read_text().splitlines(), list(p.MEMBERS))
        self.assertEqual((root / 'sdk/legacy_header_owners.txt').read_text().splitlines(), sorted(p.OWNERS.values()))
        # Independently inspect only the consumer's selected member block, not arbitrary quoted paths.
        text = (root / 'cmake/TestLegacyInstallConsumer.cmake').read_text()
        block = text.split('set(_members\n', 1)[1].split('\n)', 1)[0]
        members = [line.strip()[1:-1] for line in block.splitlines()]
        self.assertEqual(members, list(p.MEMBERS))

    def test_absolute_and_relative_q6_edges_preserve_non_tokens(self):
        sources = self.sources()
        prepared = b'#include "core/database/mysql/datetime_parameter_wire.h" // absolute\n'
        session = b'#include "datetime_parameter_descriptor.h" // relative\r\n'
        codec = b'#pragma once\n#include "datetime_wire.h"\n#include <optional>\n'
        descriptor = b'#include "core/database/mysql/parameter_receipt_shape.h"\n#include "core/database/mysql/prepared_wire.h"\n'
        sources[p.OWNERS['core/database/mysql/prepared_wire.h']] = prepared
        sources[p.OWNERS['core/database/mysql/mysql_session.h']] = session
        sources[p.OWNERS[Q6_ADDITIONS[1]]] = codec
        sources[p.OWNERS[Q6_ADDITIONS[0]]] = descriptor
        before = dict(sources)
        result = p.project(sources)
        self.assertEqual(result['core/database/mysql/prepared_wire.h'], prepared)
        self.assertEqual(result['core/database/mysql/mysql_session.h'], b'#include "core/database/mysql/datetime_parameter_descriptor.h" // relative\r\n')
        self.assertEqual(result[Q6_ADDITIONS[1]], b'#pragma once\n#include "core/database/mysql/datetime_wire.h"\n#include <optional>\n')
        self.assertEqual(result[Q6_ADDITIONS[0]], descriptor)
        self.assertEqual(sources, before)

    def test_unknown_q6_sibling_missing_and_aliased_owner_still_refused(self):
        for owner, value in (('core/database/mysql/prepared_wire.h', b'#include "core/database/mysql/datetime_parameter_unknown.h"\n'),
                             ('core/database/mysql/mysql_session.h', b'#include "datetime_parameter_unknown.h"\n')):
            with self.subTest(owner=owner):
                sources = self.sources();sources[p.OWNERS[owner]] = value;before = dict(sources)
                with self.assertRaises(p.ProjectionError):p.project(sources)
                self.assertEqual(sources, before)
        for member in Q6_ADDITIONS:
            with self.subTest(member=member):
                sources = self.sources();sources.pop(p.OWNERS[member])
                with self.assertRaises(p.ProjectionError):p.project(sources)
                owners = dict(p.OWNERS);owners[member] = owners['core/database/mysql/datetime_wire.h']
                with self.assertRaises(p.ProjectionError):p.project(self.sources(), owners)

    def test_owner_only_projection_exposes_two_missing_installed_edges(self):
        sources = self.sources()
        sources[p.OWNERS['core/database/mysql/prepared_wire.h']] = b'#include "core/database/mysql/datetime_parameter_wire.h"\n'
        sources[p.OWNERS['core/database/mysql/mysql_session.h']] = b'#include "datetime_parameter_descriptor.h"\n'
        # Counterfactual owner-only policy can rewrite references but cannot install these dependencies.
        with mock.patch.object(p, 'MEMBERS', ORIGINAL_MEMBERS):
            owner_only = p.project(sources)
        self.assertEqual(len(owner_only), 77)
        referenced = {b'core/database/mysql/datetime_parameter_wire.h', b'core/database/mysql/datetime_parameter_descriptor.h'}
        self.assertEqual({name.decode() for name in referenced} - set(owner_only), set(Q6_ADDITIONS))
        self.assertEqual(owner_only['core/database/mysql/prepared_wire.h'], b'#include "core/database/mysql/datetime_parameter_wire.h"\n')
        self.assertEqual(owner_only['core/database/mysql/mysql_session.h'], b'#include "core/database/mysql/datetime_parameter_descriptor.h"\n')
        selected = p.project(sources)
        self.assertTrue(set(Q6_ADDITIONS).issubset(selected))
        self.assertEqual(len(selected), 79)

if __name__ == '__main__':
    unittest.main()
