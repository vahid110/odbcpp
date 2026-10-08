#include "pg_database_connection.h"
#include "pg_backend_provider.h"
#include "pg_protocol_parser.h"
#include "redshift_primary_key_contract.h"
#include "redshift_catalog_query.h"
#include "odbcpp/auth/pg_credential_consumer.h"
#include <new>

#include <algorithm>
#include <charconv>
#include <unordered_set>

namespace rs::core::database::postgres {

PgDatabaseConnection::PgDatabaseConnection(
    std::unique_ptr<rs::core::transport::ITransport> transport,
    std::optional<SessionResetProfile> reset_profile, PgCatalogProfile catalog_profile)
    : GenericDatabaseConnection(std::make_unique<PgProtocolParser>(),
                                std::move(transport)), reset_profile_(reset_profile), catalog_profile_(catalog_profile) {}

BackendResult<QueryResult> PgDatabaseConnection::observe_and_decline_binary_prepared_for_test(
    std::string_view sql, std::span<const QueryParameter> params, rs::util::Deadline deadline) {
  if (catalog_profile_ != PgCatalogProfile::Redshift) return local_backend_error(
      LocalFailure::Unsupported, "Staged binary observation requires Redshift",
      BackendOperation::ExecutePrepared, session_state());
  return decline_prepared_description_candidate(sql, params, deadline);
}

BackendResult<void> PgDatabaseConnection::connect(const ConnectionSettings& settings) {
  namespace auth = rs::core::auth;
  const auto deadline = rs::util::make_deadline(settings.timeout);
  // Select before I/O; outside this first explicit-CA ordinary slice keeps its
  // existing route. A selected composition failure never retries raw password.
  if (!settings.use_ssl || settings.ssl_ca_file.empty() || !settings.ssl_ca_dir.empty() ||
      settings.password.empty() || settings.password.size() > auth::SecretBytes::max_bytes ||
      settings.host.empty() || settings.port == 0 || settings.database.empty() || settings.user.empty() ||
      deadline == rs::util::Deadline::min() || deadline == rs::util::Deadline::max())
    return connect_until(settings, deadline);
  if (is_connected()) return local_backend_error(LocalFailure::InvalidInput,
      "Database connection is already open", BackendOperation::Connect, session_state());
  const auto invalid = [&]() -> BackendResult<void> {
    return local_backend_error(LocalFailure::InvalidInput,
        "Invalid ordinary driver password composition", BackendOperation::Connect, session_state());
  };
  try {
    const auto service = catalog_profile_ == PgCatalogProfile::Redshift
        ? auth::Service::Redshift : auth::Service::PostgreSql;
    auto binding = auth::Binding::create({service, settings.host, settings.port,
        settings.database, settings.user, "pg-family-driver-password", settings.host, "verify-full"},
        {auth::SourceKind::ExternalPassword, "supplied-connection-password", "ordinary-composition-v1"},
        auth::Method::OrdinaryPassword);
    if (!binding) return invalid();
    auto request = auth::Request::create(binding.value(), deadline, rs::util::Clock::duration::zero());
    if (!request) return invalid();
    auto secret = auth::SecretBytes::create(std::as_bytes(
        std::span{settings.password.data(), settings.password.size()}));
    if (!secret) return invalid();
    auto material = auth::Material::create(binding.value(), auth::MaterialKind::OrdinaryPassword,
        settings.user, std::move(secret).value(), auth::Validity::ordinary());
    if (!material) return invalid();
    // Copy every current settings field except the secret. This avoids making a
    // transient raw password copy before an allocation-failure cleanup guard.
    ConnectionSettings selected{.host = settings.host, .user = settings.user,
        .password = {}, .database = settings.database, .port = settings.port,
        .timeout = settings.timeout, .use_ssl = settings.use_ssl,
        .ssl_ca_file = settings.ssl_ca_file, .ssl_ca_dir = settings.ssl_ca_dir,
        .response_limits = settings.response_limits, .result_limits = settings.result_limits,
        .input_limits = settings.input_limits, .startup_response_limits = settings.startup_response_limits,
        .redshift_catalog_mode = settings.redshift_catalog_mode};
    // Local binding consistency is not issuer proof, expiry, or pooling authority.
    return auth::connect_bound_ordinary_password_until(*this, selected, request.value(), material.value());
  } catch (const std::bad_alloc&) {
    disconnect();
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::AllocationFailure),
                       "Ordinary driver password composition allocation failed"};
    error.operation = BackendOperation::Connect;
    error.session_state = SessionState::Disconnected;
    error.disposition = SessionDisposition::Retire;
    return error;
  } catch (...) {
    disconnect();
    return invalid();
  }
}

BackendResult<void> PgDatabaseConnection::connect_until(
    const ConnectionSettings& settings, rs::util::Deadline deadline) {
  if (catalog_profile_ == PgCatalogProfile::Redshift && is_connected()) return local_backend_error(LocalFailure::InvalidInput,
      "Database connection is already open", BackendOperation::Connect, session_state());
  if (settings.redshift_catalog_mode &&
      (catalog_profile_ != PgCatalogProfile::Redshift ||
       (*settings.redshift_catalog_mode != RedshiftCatalogMode::Show &&
        *settings.redshift_catalog_mode != RedshiftCatalogMode::Legacy)))
    return local_backend_error(LocalFailure::InvalidInput,
        "Invalid Redshift catalog mode for this provider", BackendOperation::Connect, session_state());
  auto result = GenericDatabaseConnection::connect_until(settings, deadline);
  if (result) catalog_mode_ = settings.redshift_catalog_mode.value_or(RedshiftCatalogMode::Show);
  return result;
}

BackendResult<void> PgDatabaseConnection::check_health(rs::util::Deadline deadline) {
  auto result = execute_query("SELECT 1", deadline);
  if (result.has_error()) {
    auto error = std::move(result.backend_error());
    error.operation = BackendOperation::CheckHealth;
    return error;
  }
  const auto snapshot = result.session_snapshot();
  // Validate the fixed probe, not merely receipt of ReadyForQuery. Unexpected
  // successful payload/state is a protocol failure and cannot authorize reuse.
  if (result->columns.size() != 1 || result->rows.size() != 1 ||
      result->rows[0].size() != 1 || result->rows[0][0] != "1" ||
      !result->cell_errors.empty() || !result->additional_results.empty() ||
      result->error || (snapshot.state != SessionState::Idle &&
                        snapshot.state != SessionState::Transaction)) {
    disconnect();
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
                       "Unexpected health probe response"};
    error.operation = BackendOperation::CheckHealth;
    error.session_state = SessionState::Disconnected;
    error.disposition = SessionDisposition::Retire;
    return error;
  }
  return BackendResult<void>{snapshot};
}

bool PgDatabaseConnection::selects_catalog_request(const CatalogRequest& request) const noexcept {
  if (catalog_profile_ != PgCatalogProfile::Redshift) return false;
  if (std::holds_alternative<PrimaryKeysCatalogRequest>(request)) return true;
  if (const auto* columns=std::get_if<ColumnsCatalogRequest>(&request))
    return catalog_mode()==RedshiftCatalogMode::Show && redshift_column_names_are_literal(*columns);
  const auto* tables = std::get_if<TablesCatalogRequest>(&request);
  return tables && catalog_mode() == RedshiftCatalogMode::Show &&
      (tables->mode == TablesCatalogRequest::Mode::Schemas || redshift_table_schema_is_literal(*tables));
}

BackendResult<QueryResult> PgDatabaseConnection::execute_catalog(
    const CatalogRequest& request, rs::util::Deadline deadline) {
  if (!is_connected()) {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::NotConnected),
                       "Database session is not connected"};
    error.operation = BackendOperation::ExecuteCatalog;
    error.session_state = SessionState::Disconnected;
    error.disposition = SessionDisposition::Retire;
    return error;
  }
  const auto unsupported = [this] {
    return local_backend_error(LocalFailure::Unsupported,
        "Exact modern Redshift primary-key discovery is unavailable",
        BackendOperation::ExecuteCatalog, session_state());
  };
  const auto* tables = std::get_if<TablesCatalogRequest>(&request);
  if (catalog_profile_ == PgCatalogProfile::Redshift && tables &&
      tables->mode == TablesCatalogRequest::Mode::Schemas &&
      catalog_mode() == RedshiftCatalogMode::Show) {
    // Selection precedes capability: refusal must never choose generated SQL.
    const auto schema_unsupported = [this] {
      return local_backend_error(LocalFailure::Unsupported,
          "Modern Redshift schema discovery is unavailable",
          BackendOperation::ExecuteCatalog, session_state());
    };
    const auto capability = get_parameter("show_discovery");
    std::uint32_t version = 0;
    if (capability.empty() || capability.size() > 10 ||
        capability.find_first_not_of("0123456789") != std::string::npos)
      return schema_unsupported();
    const auto parsed = std::from_chars(capability.data(),
        capability.data() + capability.size(), version);
    if (parsed.ec != std::errc{} || parsed.ptr != capability.data() + capability.size() ||
        version < 4) return schema_unsupported();
    const auto expired = [deadline] { return rs::util::Clock::now() >= deadline; };
    const auto timeout = [] {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),
          "Schema discovery deadline expired"};
      error.operation = BackendOperation::ExecuteCatalog;
      return BackendResult<QueryResult>{std::move(error)};
    };
    if (expired()) return timeout();
    auto identity = execute_query("SELECT current_database() AS database_name", deadline);
    if (!identity) return identity.backend_error();
    if (expired()) return timeout();
    auto database = redshift_schema_database(std::move(identity));
    if (expired()) return timeout();
    if (!database) return database.backend_error();
    auto response = execute_query(redshift_show_schemas_command(*database), deadline);
    if (!response) return response.backend_error();
    if (expired()) return timeout();
    auto normalized = normalize_redshift_schemas(*database, std::move(response));
    if (expired()) return timeout();
    return normalized;
  }
  if (catalog_profile_ == PgCatalogProfile::Redshift && tables &&
      catalog_mode() == RedshiftCatalogMode::Show && redshift_table_schema_is_literal(*tables)) {
    const auto schema = redshift_table_schema(*tables);
    if (!schema) return local_backend_error(LocalFailure::InvalidInput,
        "Invalid Redshift table schema pattern", BackendOperation::ExecuteCatalog, session_state());
    const auto capability = get_parameter("show_discovery");
    std::uint32_t version = 0;
    const auto parsed = std::from_chars(capability.data(), capability.data()+capability.size(), version);
    if (capability.empty() || capability.size()>10 || parsed.ec!=std::errc{} ||
        parsed.ptr!=capability.data()+capability.size() || version<4)
      return local_backend_error(LocalFailure::Unsupported,
          "Modern Redshift table discovery is unavailable", BackendOperation::ExecuteCatalog, session_state());
    const auto expired = [deadline] { return rs::util::Clock::now() >= deadline; };
    const auto timeout = [] {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout), "Table discovery deadline expired"};
      error.operation = BackendOperation::ExecuteCatalog;
      return BackendResult<QueryResult>{std::move(error)};
    };
    if (expired()) return timeout();
    auto identity = execute_query("SELECT current_database() AS database_name", deadline);
    if (!identity) return identity.backend_error();
    if (expired()) return timeout();
    auto database = redshift_schema_database(std::move(identity));
    if (expired()) return timeout();
    if (!database) return database.backend_error();
    auto response = execute_query(redshift_show_tables_command(*database,*schema),deadline);
    if (!response) return response.backend_error();
    if (expired()) return timeout();
    auto normalized = normalize_redshift_tables(*database,*schema,*tables,std::move(response));
    if (expired()) return timeout();
    return normalized;
  }
  const auto* columns=std::get_if<ColumnsCatalogRequest>(&request);
  if (catalog_profile_==PgCatalogProfile::Redshift && columns &&
      catalog_mode()==RedshiftCatalogMode::Show && redshift_column_names_are_literal(*columns)) {
    const auto names=redshift_column_names(*columns);
    if (!names) return local_backend_error(LocalFailure::InvalidInput,
        "Invalid Redshift column identifier or pattern",BackendOperation::ExecuteCatalog,session_state());
    const auto capability=get_parameter("show_discovery");std::uint32_t version=0;
    const auto parsed=std::from_chars(capability.data(),capability.data()+capability.size(),version);
    if (capability.empty() || capability.size()>10 || parsed.ec!=std::errc{} ||
        parsed.ptr!=capability.data()+capability.size() || version<4)
      return local_backend_error(LocalFailure::Unsupported,
          "Modern Redshift column discovery is unavailable",BackendOperation::ExecuteCatalog,session_state());
    const auto expired=[deadline]{return rs::util::Clock::now()>=deadline;};
    const auto timeout=[] {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),"Column discovery deadline expired"};
      error.operation=BackendOperation::ExecuteCatalog;return BackendResult<QueryResult>{std::move(error)};
    };
    if (expired())return timeout();
    auto identity=execute_query("SELECT current_database() AS database_name",deadline);
    if (!identity) return identity.backend_error();
    if (expired()) return timeout();
    auto database=redshift_schema_database(std::move(identity));
    if (expired()) return timeout();
    if (!database) return database.backend_error();
    auto response=execute_query(redshift_show_columns_command(*database,names->first,names->second),deadline);
    if (!response) return response.backend_error();
    if (expired()) return timeout();
    auto normalized=normalize_redshift_columns(*database,names->first,names->second,*columns,std::move(response));
    if (expired()) return timeout();
    return normalized;
  }
  const auto* keys = std::get_if<PrimaryKeysCatalogRequest>(&request);
  if (catalog_profile_ != PgCatalogProfile::Redshift || !keys) return unsupported();
  auto plan = redshift_primary_key_plan(keys->catalog, keys->schema, keys->table);
  if (!plan) {
    return local_backend_error(LocalFailure::InvalidInput,
        "Exact database, schema and table identifiers are required",
        BackendOperation::ExecuteCatalog, session_state());
  }
  if (catalog_mode() == RedshiftCatalogMode::Legacy) {
    // Exact equality and index-attribute order are this scoped legacy contract.
    // No capability probe, identifier interpolation or SHOW retry is allowed.
    const std::vector<QueryParameter> values{
        {plan->database, QueryParameterType::Text},
        {plan->schema, QueryParameterType::Text},
        {plan->table, QueryParameterType::Text}};
    return normalize_redshift_primary_keys(*plan, execute_prepared(
        "SELECT current_database() AS database_name, n.nspname AS schema_name, "
        "ct.relname AS table_name, a.attname AS column_name, "
        "a.attnum AS key_seq, ci.relname AS pk_name "
        "FROM pg_catalog.pg_namespace n, pg_catalog.pg_class ct, "
        "pg_catalog.pg_class ci, pg_catalog.pg_attribute a, pg_catalog.pg_index i "
        "WHERE ct.oid=i.indrelid AND ci.oid=i.indexrelid AND a.attrelid=ci.oid "
        "AND i.indisprimary AND ct.relnamespace=n.oid "
        "AND current_database()=? AND n.nspname=? AND ct.relname=? "
        "ORDER BY schema_name, table_name, key_seq", values, deadline));
  }
  // Only authenticated ParameterStatus selects this path. No discovery SQL,
  // default identifiers or inherited PostgreSQL fallback is permitted.
  const auto capability = get_parameter("show_discovery");
  if (capability.empty() || capability.size() > 10 ||
      capability.find_first_not_of("0123456789") != std::string::npos)
    return unsupported();
  std::uint32_t version = 0;
  const auto parsed = std::from_chars(capability.data(),
      capability.data() + capability.size(), version);
  if (parsed.ec != std::errc{} || parsed.ptr != capability.data() + capability.size() ||
      version < 4) return unsupported();
  const std::vector<QueryParameter> parameters{
      {plan->database, QueryParameterType::Unspecified},
      {plan->schema, QueryParameterType::Unspecified},
      {plan->table, QueryParameterType::Unspecified}};
  // Prepared execution (including any existing type resolution) retains the
  // caller's absolute deadline. Native errors and final snapshots stay owning.
  return normalize_redshift_primary_keys(*plan, execute_prepared(
      "SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE ?.?.?;", parameters, deadline));
}

BackendResult<ResolvedTypeMap> PgDatabaseConnection::resolve_types(
    std::span<const std::uint32_t> ids, rs::util::Deadline deadline) {
  if (catalog_profile_ == PgCatalogProfile::Redshift &&
      std::find(ids.begin(), ids.end(), 6551u) != ids.end()) {
    // This rejects unqualified parameter metadata, not execution before I/O:
    // the combined prepared exchange may already have sent Execute.
    return local_backend_error(LocalFailure::Unsupported,
        "Redshift VARBYTE parameters are not qualified", BackendOperation::ResolveTypes,
        session_state());
  }
  ResolvedTypeMap resolved;
  std::vector<std::uint32_t> unresolved;
  for (const auto id : ids) {
    const auto type = describe_type(id, -1, -1);
    const auto inserted = resolved.emplace(id, type).second;
    if (inserted && id != 0 && !type.known) unresolved.push_back(id);
  }
  if (unresolved.empty()) return resolved;
  std::sort(unresolved.begin(), unresolved.end());

  std::string query =
      "WITH RECURSIVE type_chain(original_oid, type_oid, base_oid, "
      "type_modifier) AS ("
      "SELECT oid, oid, typbasetype, typtypmod "
      "FROM pg_catalog.pg_type WHERE oid IN (";
  for (std::size_t index = 0; index < unresolved.size(); ++index) {
    if (index != 0) query += ',';
    query += std::to_string(unresolved[index]);
  }
  query +=
      ") UNION ALL SELECT chain.original_oid, base.oid, base.typbasetype, "
      "CASE WHEN chain.type_modifier >= 0 THEN chain.type_modifier "
      "ELSE base.typtypmod END "
      "FROM type_chain AS chain JOIN pg_catalog.pg_type AS base "
      "ON base.oid = chain.base_oid) "
      "SELECT original_oid::text, type_oid::text, type_modifier::text "
      "FROM type_chain "
      "WHERE base_oid = 0";

  auto types = execute_query(query, deadline);
  if (types.has_error()) {
    auto error = std::move(types.backend_error());
    error.operation = BackendOperation::ResolveTypes;
    return error;
  }

  const auto parse_number = [](const std::string& value, auto& number) {
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), number);
    return error == std::errc{} && end == value.data() + value.size();
  };
  std::unordered_set<std::uint32_t> seen;
  for (const auto& row : types->rows) {
    std::uint32_t original = 0, base = 0;
    std::int32_t modifier = -1;
    if (row.size() != 3 || !row[0] || !row[1] || !row[2] ||
        !parse_number(*row[0], original) || !parse_number(*row[1], base) ||
        !parse_number(*row[2], modifier) || base == 0 ||
        !std::binary_search(unresolved.begin(), unresolved.end(), original) ||
        !seen.insert(original).second) {
      // The query has completed and drained. Invalid metadata does not itself
      // imply an ambiguous transport or justify retiring an idle session.
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),
                        "Data source returned invalid parameter type metadata"};
      error.error_class = BackendErrorClass::InvalidMetadata;
      error.operation = BackendOperation::ResolveTypes;
      error.session_state = session_state();
      error.disposition = error.session_state == SessionState::Disconnected ? SessionDisposition::Retire :
          error.session_state == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired;
      return error;
    }
    resolved.at(original) = describe_type(base, -1, modifier);
  }
  return resolved;
}

} // namespace rs::core::database::postgres
