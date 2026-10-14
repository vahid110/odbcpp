#include "pg_database_connection.h"
#include "pg_backend_provider.h"
#include "pg_protocol_parser.h"
#include "redshift_primary_key_contract.h"
#include "redshift_foreign_key_contract.h"
#include "redshift_catalog_query.h"
#include "odbcpp/auth/pg_credential_consumer.h"
#include "odbcpp/util/utf8.h"
#include <new>

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_set>
#include <tuple>
#include <type_traits>

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
        .redshift_catalog_mode = settings.redshift_catalog_mode,
        .application_name = settings.application_name,
        .read_only = settings.read_only,
        .character_metadata = settings.character_metadata,
        .detailed_table_types = settings.detailed_table_types,
        .catalog_database_scope = settings.catalog_database_scope};
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
  if (settings.catalog_database_scope != CatalogDatabaseScope::CurrentDatabase &&
      settings.catalog_database_scope != CatalogDatabaseScope::VisibleDatabases)
    return local_backend_error(LocalFailure::InvalidInput,
        "Invalid metadata database scope", BackendOperation::Connect, session_state());
  if (settings.catalog_database_scope == CatalogDatabaseScope::VisibleDatabases &&
      catalog_profile_ != PgCatalogProfile::Redshift)
    return local_backend_error(LocalFailure::Unsupported,
        "Visible metadata databases require Redshift", BackendOperation::Connect, session_state());
  if (settings.character_metadata.varchar_threshold >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      (catalog_profile_ == PgCatalogProfile::Redshift &&
       settings.character_metadata.long_varchar_size > 65535))
    return local_backend_error(LocalFailure::InvalidInput,
        "Invalid character metadata sizing", BackendOperation::Connect, session_state());
  if (settings.character_metadata.boolean_as_character && catalog_profile_ != PgCatalogProfile::Redshift)
    return local_backend_error(LocalFailure::Unsupported,
        "BoolsAsChar requires Redshift", BackendOperation::Connect, session_state());
  if (!settings.detailed_table_types && catalog_profile_ != PgCatalogProfile::Redshift)
    return local_backend_error(LocalFailure::Unsupported,
        "EnableTableTypes requires Redshift", BackendOperation::Connect, session_state());
  if (settings.read_only && catalog_profile_ != PgCatalogProfile::Redshift)
    return local_backend_error(LocalFailure::Unsupported,
        "ReadOnly requires Redshift", BackendOperation::Connect, session_state());
  if (catalog_profile_ == PgCatalogProfile::Redshift && is_connected()) return local_backend_error(LocalFailure::InvalidInput,
      "Database connection is already open", BackendOperation::Connect, session_state());
  if (settings.redshift_catalog_mode &&
      (catalog_profile_ != PgCatalogProfile::Redshift ||
       (*settings.redshift_catalog_mode != RedshiftCatalogMode::Show &&
        *settings.redshift_catalog_mode != RedshiftCatalogMode::Legacy)))
    return local_backend_error(LocalFailure::InvalidInput,
        "Invalid Redshift catalog mode for this provider", BackendOperation::Connect, session_state());
  auto result = GenericDatabaseConnection::connect_until(settings, deadline);
  if (result && settings.read_only) {
    // Native setup belongs to this backend and shares the original login budget.
    // A failed or unexpected completion must never publish an unrestricted session.
    const auto retire = [this](BackendError error) -> BackendResult<void> {
      disconnect();
      error.operation = BackendOperation::Connect;
      error.session_state = SessionState::Disconnected;
      error.disposition = SessionDisposition::Retire;
      return error;
    };
    try {
      auto setup = execute_query("SET READONLY=1", deadline);
      if (setup.has_error()) return retire(std::move(setup.backend_error()));
      if (setup->error) return retire(std::move(*setup->error));
      if (setup->execution_result_shape != ExecutionResultShape::NoResultSet ||
          !setup->columns.empty() || !setup->rows.empty() ||
          !setup->cell_errors.empty() || !setup->additional_results.empty() ||
          setup.session_snapshot() != SessionSnapshot{
              SessionState::Idle, SessionDisposition::Reusable})
        return retire(BackendError{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
                                  "Unexpected ReadOnly setup response"});
      result = BackendResult<void>{setup.session_snapshot()};
    } catch (const std::bad_alloc&) {
      return retire(BackendError{rs::util::make_error_code(rs::util::DbErrorCode::AllocationFailure),
                                "ReadOnly setup memory allocation failed"});
    } catch (...) {
      return retire(BackendError{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
                                "ReadOnly setup failed"});
    }
  }
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
  if (std::holds_alternative<PrimaryKeysCatalogRequest>(request) ||
      std::holds_alternative<ForeignKeysCatalogRequest>(request) ||
      std::holds_alternative<ProceduresCatalogRequest>(request) ||
      std::holds_alternative<ProcedureColumnsCatalogRequest>(request)) return true;
  // Exact identifier requests are selected before availability checks: no PG fallback.
  if (const auto* tables = std::get_if<TablesCatalogRequest>(&request);
      tables && tables->mode == TablesCatalogRequest::Mode::Tables &&
      !tables->name_matches.existing()) return true;
  if (const auto* columns = std::get_if<ColumnsCatalogRequest>(&request);
      columns && !columns->name_matches.existing()) return true;
  if (const auto* columns=std::get_if<ColumnsCatalogRequest>(&request))
    return catalog_mode()==RedshiftCatalogMode::Show &&
        (redshift_column_names_are_literal(*columns) || redshift_columns_schema_wide(*columns));
  const auto* tables = std::get_if<TablesCatalogRequest>(&request);
  if (tables && catalog_mode() == RedshiftCatalogMode::Show &&
      tables->mode == TablesCatalogRequest::Mode::Catalogs) {
    // Preserve the existing generated current-database enumeration in manual
    // transactions. Only effective visible scope requires dedicated discovery.
    try {
      const auto scope = redshift_catalog_database_scope(catalog_database_scope(),
          get_parameter_status("datashare_enabled"), get_parameter_status("external_database"));
      return !scope || *scope == CatalogDatabaseScope::VisibleDatabases;
    } catch (...) {
      // Select the guarded executor on observation failure; never silently
      // downgrade to a current-only generated query.
      return true;
    }
  }
  return tables && catalog_mode() == RedshiftCatalogMode::Show &&
      (tables->mode == TablesCatalogRequest::Mode::Schemas || redshift_table_schema_is_literal(*tables));
}

BackendResult<QueryResult> PgDatabaseConnection::execute_database_discovery(
    const CatalogRequest& request, const CatalogRequest& semantic_request,
    CatalogDatabaseScope scope, rs::util::Deadline deadline) {
  const auto unavailable = [this] {
    return local_backend_error(LocalFailure::Unsupported, "Modern Redshift database discovery is unavailable",
        BackendOperation::ExecuteCatalog, session_state());
  };
  if (session_state() != SessionState::Idle) return unavailable();
  const auto* enumeration = std::get_if<TablesCatalogRequest>(&request);
  if (scope == CatalogDatabaseScope::VisibleDatabases ||
      !enumeration || enumeration->mode != TablesCatalogRequest::Mode::Catalogs) {
    const auto capability = get_parameter("show_discovery");
    std::uint32_t version = 0;
    const auto parsed = std::from_chars(capability.data(), capability.data() + capability.size(), version);
    if (capability.empty() || capability.size() > 10 || parsed.ec != std::errc{} ||
        parsed.ptr != capability.data() + capability.size() || version < 4) return unavailable();
  }
  const auto expired = [deadline] { return rs::util::Clock::now() >= deadline; };
  const auto timeout = [] {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout), "Database discovery deadline expired"};
    error.operation = BackendOperation::ExecuteCatalog;
    return error;
  };
  const auto malformed = [] {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError), "Invalid database discovery context"};
    error.operation = BackendOperation::ExecuteCatalog;
    error.error_class = BackendErrorClass::InvalidMetadata;
    error.disposition = SessionDisposition::Retire;
    return error;
  };
  RedshiftRoutineBudget budget{catalog_response_limits(), catalog_result_limits()};
  const auto exchange = [&](std::string_view sql) -> BackendResult<QueryResult> {
    if (expired()) return timeout();
    if (!budget.exchange()) return redshift_routine_limit();
    auto result = execute_query(sql, deadline);
    if (!result) return result.backend_error();
    if (expired()) return timeout();
    if (result.session_snapshot().state != SessionState::Idle ||
        result.session_snapshot().disposition != SessionDisposition::Reusable) {
      auto error=malformed();error.session_state=result.session_snapshot().state;return error;
    }
    return result;
  };
  const auto* tables = std::get_if<TablesCatalogRequest>(&request);
  const auto* columns = std::get_if<ColumnsCatalogRequest>(&request);
  if (!tables && !columns) return unavailable();
  if (columns && redshift_columns_schema_wide(*columns)) {
    // Selected intent never falls back after capability/state validation or after
    // a native exchange. Returned names are exact identifiers, not patterns.
    const auto& original=std::get<ColumnsCatalogRequest>(semantic_request);
    if (!budget.name(*columns->schema) ||
        !budget.retain({0,0,columns->schema->size()+sizeof(std::string),0,columns->schema->size()}))
      return redshift_routine_limit();
    const auto schema_name=redshift_columns_schema(*columns);
    if (!schema_name) return local_backend_error(LocalFailure::InvalidInput,
        "Invalid schema-wide column names",BackendOperation::ExecuteCatalog,session_state());
    // A valid literal may simply not match the empty preflight context; the
    // Result's error state, not its contained match value, validates the text.
    auto valid_catalog=redshift_database_matches({},original.catalog,false);
    if (!valid_catalog) return local_backend_error(LocalFailure::InvalidInput,
        valid_catalog.error_message(),BackendOperation::ExecuteCatalog,session_state());
    if (original.catalog && !budget.name(*original.catalog)) return redshift_routine_limit();
    // The validated caller pattern remains live throughout discovery/fanout;
    // it is never substituted for a returned exact native table identity.
    if (columns->table && (!budget.name(*columns->table) ||
        !budget.retain({0,0,columns->table->size()+sizeof(std::string),0,columns->table->size()})))
      return redshift_routine_limit();
    const auto native_exchange=[&](std::string_view database,std::string_view schema,
        const std::string* table=nullptr) -> BackendResult<QueryResult> {
      const auto max=std::numeric_limits<std::size_t>::max();
      std::size_t bytes=64;
      for (const auto name:{database,schema,table?std::string_view{*table}:std::string_view{}}) {
        if (name.size()>(max-bytes)/2) return redshift_routine_limit();
        bytes+=name.size()*2;
      }
      if (!budget.retain({0,0,bytes,0,0})) return redshift_routine_limit();
      BackendResult<QueryResult> result=malformed();
      {
        if (expired()) return timeout();
        const auto command=table?redshift_show_columns_command(database,schema,*table):
            redshift_show_tables_command(database,schema);
        result=exchange(command);
      }
      budget.release({0,0,bytes,0,0});
      return result;
    };
    std::vector<std::string> databases;
    RedshiftRoutineUsage database_names;
    {
      auto response=exchange(scope==CatalogDatabaseScope::CurrentDatabase ?
          "SELECT current_database() AS database_name" : "SHOW DATABASES;");
      if (!response) return response.backend_error();
      const auto raw=redshift_routine_schema_usage(*response);
      // The existing native database decoder has a set plus owning vector.
      // Admit both and their payload before calling it; raw dies in this scope.
      if (raw.bytes>(std::numeric_limits<std::size_t>::max()-1024)/3) return redshift_routine_limit();
      RedshiftRoutineUsage scratch{response->rows.size(),response->rows.size(),raw.bytes*3+1024,
          response->rows.size()*2,0};
      if (!budget.description(response->columns.size())) return redshift_routine_limit();
      for (std::size_t c=0;c<response->columns.size();++c) {
        if (!budget.name(response->columns[c].name)) return redshift_routine_limit();
        if (response->columns[c].name!="database_name") continue;
        for (const auto& row:response->rows) {
          if (c>=row.size() || !row[c]) continue; // Decoder still rejects malformed shape/NULL.
          if (!budget.name(*row[c]) || row[c]->size()>(std::numeric_limits<std::size_t>::max()-scratch.name_bytes)/3)
            return redshift_routine_limit();
          scratch.name_bytes+=row[c]->size()*3;
        }
      }
      if (!budget.retain(raw) || !budget.retain(scratch)) return redshift_routine_limit();
      if (expired()) return timeout();
      if (scope==CatalogDatabaseScope::CurrentDatabase) {
        auto database=redshift_schema_database(std::move(response));
        if (!database) return database.backend_error();
        databases.push_back(std::move(*database));
      } else {
        auto selected=normalize_redshift_databases(std::move(response));
        if (!selected) return selected.backend_error();
        databases=std::move(*selected);
      }
      database_names={databases.size(),databases.size(),databases.capacity()*sizeof(std::string),databases.size(),0};
      for (const auto& database:databases) {
        if (!budget.name(database) || database.size()>scratch.bytes-database_names.bytes) return redshift_routine_limit();
        database_names.bytes+=database.size();database_names.name_bytes+=database.size();
      }
      if (database_names.bytes>scratch.bytes || database_names.name_bytes>scratch.name_bytes) return redshift_routine_limit();
      // The decoder has destroyed its raw response and set, before releasing.
      budget.release(raw);
      budget.release({scratch.rows-database_names.rows,scratch.cells-database_names.cells,
          scratch.bytes-database_names.bytes,scratch.metadata-database_names.metadata,
          scratch.name_bytes-database_names.name_bytes});
    }
    auto headers=redshift_columns_headers(budget);
    if (!headers) return headers.backend_error();
    QueryResult output=std::move(*headers);
    bool first=true;
    std::size_t remaining=0;
    for (const auto& database:databases) if (!original.catalog || *original.catalog==database) ++remaining;
    if (!budget.exchanges_available(remaining)) return redshift_routine_limit();
    for (const auto& database:databases) {
      if (original.catalog && *original.catalog!=database) continue;
      if (expired()) return timeout();
      --remaining;
      RedshiftColumnTables selected;
      {
        auto response=native_exchange(database,*schema_name);
        if (!response) return response.backend_error();
        auto tables_selected=redshift_column_tables(database,*schema_name,columns->table,response,budget,deadline);
        if (!tables_selected) return tables_selected.backend_error();
        selected=std::move(*tables_selected);
      } // Raw table rows have actually been destroyed.
      budget.release(selected.raw_retained);
      if (selected.names.size()>std::numeric_limits<std::size_t>::max()-remaining ||
          !budget.exchanges_available(selected.names.size()+remaining)) return redshift_routine_limit();
      const auto selected_usage=selected.retained;
      {
        // Destroy the selection before releasing its charge at the database boundary.
        auto owned_tables=std::move(selected.names);
        for (const auto& table:owned_tables) {
          if (expired()) return timeout();
          RedshiftRoutineUsage destroyed;
          {
            auto response=native_exchange(database,*schema_name,&table);
            if (!response) return response.backend_error();
            auto part=redshift_columns_part(database,*schema_name,table,*columns,std::move(response),budget,deadline);
            if (!part) return part.backend_error();
            QueryResult old_headers;
            if (first) {
              old_headers.columns=std::move(output.columns);
              output.columns=std::move(part->columns);first=false;
            } else {
              if (part->columns.size()!=output.columns.size()) return malformed();
              for (std::size_t i=0;i<output.columns.size();++i) {
                const auto& incoming=part->columns[i];auto& retained=output.columns[i];
                if (incoming.name!=retained.name || !incoming.normalized_type || !retained.normalized_type) return malformed();
                const auto& from=*incoming.normalized_type;auto& to=*retained.normalized_type;
                if (from.type!=to.type || from.known!=to.known || from.decimal_digits!=to.decimal_digits ||
                    from.declared_type!=to.declared_type || from.declared_column_size!=to.declared_column_size) return malformed();
                to.column_size=from.column_size==0 || to.column_size==0 ? 0 : std::max(to.column_size,from.column_size);
              }
              old_headers.columns=std::move(part->columns);
            }
            // Admit the new row index while the old index and per-table index
            // remain live. Moving cells does not destroy their owning payload.
            const auto count=output.rows.size()+part->rows.size();
            if (count<output.rows.size() || count>std::numeric_limits<std::size_t>::max()/sizeof(ResultRow)) return redshift_routine_limit();
            if (count>output.rows.capacity()) {
              const auto old_capacity=output.rows.capacity();
              if (!budget.retain({0,0,count*sizeof(ResultRow),0,0})) return redshift_routine_limit();
              if (expired()) return timeout();
              output.rows.reserve(count);
              budget.release({0,0,old_capacity*sizeof(ResultRow),0,0});
            }
            destroyed=redshift_routine_schema_usage(old_headers);
            destroyed.bytes+=part->rows.capacity()*sizeof(ResultRow);
            for (auto& row:part->rows) output.rows.push_back(std::move(row));
          } // Headers and the emptied per-table row index have been destroyed.
          budget.release(destroyed);
        }
      }
      budget.release(selected_usage);
    }
    std::sort(output.rows.begin(),output.rows.end(),[](const ResultRow& a,const ResultRow& b) {
      if (std::tie(a[0],a[1],a[2])!=std::tie(b[0],b[1],b[2]))
        return std::tie(a[0],a[1],a[2])<std::tie(b[0],b[1],b[2]);
      std::int32_t left=0,right=0;
      std::from_chars(a[16]->data(),a[16]->data()+a[16]->size(),left);
      std::from_chars(b[16]->data(),b[16]->data()+b[16]->size(),right);return left<right;
    });
    if (expired()) return timeout();
    return BackendResult<QueryResult>{std::move(output),{SessionState::Idle,SessionDisposition::Reusable}};
  }
  std::optional<std::string> catalog;
  bool pattern = false;
  if (const auto* semantic = std::get_if<TablesCatalogRequest>(&semantic_request)) {
    if (semantic->mode == TablesCatalogRequest::Mode::Tables) {
      catalog = semantic->catalog;
      pattern = semantic->name_matches.catalog == CatalogNameMatch::Existing;
    }
  } else catalog = std::get<ColumnsCatalogRequest>(semantic_request).catalog;
  if (catalog && (!budget.name(*catalog) || !budget.retain({0, 0, catalog->size(), 0, catalog->size()})))
    return redshift_routine_limit();
  const auto schema = tables && tables->mode == TablesCatalogRequest::Mode::Tables ?
      redshift_table_schema(*tables) : std::optional<std::string>{};
  const auto names = columns ? redshift_column_names(*columns) :
      std::optional<std::pair<std::string, std::string>>{};
  if ((tables && tables->mode == TablesCatalogRequest::Mode::Tables && !schema) || (columns && !names))
    return local_backend_error(LocalFailure::InvalidInput, "Invalid database discovery names",
        BackendOperation::ExecuteCatalog, session_state());
  auto valid_catalog = redshift_database_matches({}, catalog, pattern);
  if (!valid_catalog) return local_backend_error(LocalFailure::InvalidInput, valid_catalog.error_message(),
      BackendOperation::ExecuteCatalog, session_state());
  if (schema && (!budget.name(*schema) || !budget.retain({0, 0, schema->size(), 0, schema->size()})))
    return redshift_routine_limit();
  if (names) {
    for (const auto& name : {names->first, names->second})
      if (!budget.name(name) || !budget.retain({0, 0, name.size(), 0, name.size()})) return redshift_routine_limit();
  }
  std::vector<std::string> databases;
  std::uint64_t database_width = 0;
  {
    auto response = exchange(scope == CatalogDatabaseScope::CurrentDatabase ?
        "SELECT current_database() AS database_name" : "SHOW DATABASES;");
    if (!response) return response.backend_error();
    const auto usage = redshift_routine_usage(*response);
    if (!budget.retain(usage)) return redshift_routine_limit();
    for (const auto& column : response->columns) {
      if (column.name == "database_name" && column.normalized_type)
        database_width = declared_character_metadata(*column.normalized_type).column_size;
    }
    if (scope == CatalogDatabaseScope::CurrentDatabase) {
      auto database = redshift_schema_database(std::move(response));
      if (!database) return database.backend_error();
      databases.push_back(std::move(*database));
    } else {
      auto selected = normalize_redshift_databases(std::move(response));
      if (!selected) return selected.backend_error();
      databases = std::move(*selected);
    }
    for (const auto& database : databases)
      if (!budget.name(database) || !budget.retain({0, 0, database.size(), 0, database.size()}))
        return redshift_routine_limit();
    budget.release(usage);
  }
  QueryResult output;
  if (tables) {
    for (const auto* name : {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS"})
      output.columns.push_back({name, NativeTypeInfo{ScalarType::VarChar, 0, 0, true}});
  } else {
    constexpr std::array<const char*, 18> column_names{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "DATA_TYPE",
        "TYPE_NAME", "COLUMN_SIZE", "BUFFER_LENGTH", "DECIMAL_DIGITS", "NUM_PREC_RADIX", "NULLABLE", "REMARKS",
        "COLUMN_DEF", "SQL_DATA_TYPE", "SQL_DATETIME_SUB", "CHAR_OCTET_LENGTH", "ORDINAL_POSITION", "IS_NULLABLE"};
    for (std::size_t i = 0; i < column_names.size(); ++i) {
      const bool text = i < 4 || i == 5 || i == 11 || i == 12 || i == 17;
      const bool integer = i == 6 || i == 7 || i == 15 || i == 16;
      output.columns.push_back({column_names[i], NativeTypeInfo{text ? ScalarType::VarChar :
          integer ? ScalarType::Integer : ScalarType::SmallInt, text ? 0u : integer ? 10u : 5u, 0, true}});
    }
  }
  if (!budget.description(output.columns.size()) || !budget.retain(redshift_routine_usage(output)))
    return redshift_routine_limit();
  if (tables && tables->mode == TablesCatalogRequest::Mode::Catalogs) {
    output.columns[0].normalized_type->column_size = database_width;
    for (const auto& database : databases) {
      if (expired()) return timeout();
      ResultRow row{database, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
      if (!budget.retain({1, 5, database.size(), 0, 0})) return redshift_routine_limit();
      output.rows.push_back(std::move(row));
    }
  } else {
    bool first = true;
    for (const auto& database : databases) {
      if (expired()) return timeout();
      auto matches = redshift_database_matches(database, catalog, pattern);
      if (!matches) return local_backend_error(LocalFailure::InvalidInput, matches.error_message(),
          BackendOperation::ExecuteCatalog, session_state());
      if (!*matches) continue;
      auto response = exchange(tables ? (tables->mode == TablesCatalogRequest::Mode::Schemas ?
          redshift_show_schemas_command(database) : redshift_show_tables_command(database, *schema)) :
          redshift_show_columns_command(database, names->first, names->second));
      if (!response) return response.backend_error();
      const auto usage = redshift_routine_usage(*response);
      if (!budget.retain(usage)) return redshift_routine_limit();
      auto part = tables ? (tables->mode == TablesCatalogRequest::Mode::Schemas ?
          normalize_redshift_schemas(database, std::move(response)) :
          normalize_redshift_tables(database, *schema, *tables, std::move(response), catalog_detailed_table_types())) :
          normalize_redshift_columns(database, names->first, names->second, *columns, std::move(response));
      if (!part) return part.backend_error();
      if (tables && tables->mode == TablesCatalogRequest::Mode::Schemas) {
        part->columns[0].normalized_type->column_size = database_width;
        for (auto& row : part->rows) row[0] = database;
      }
      if (!budget.retain(redshift_routine_usage(*part))) return redshift_routine_limit();
      budget.release(usage);
      if (first) {
        QueryResult old_headers; old_headers.columns = std::move(output.columns);
        output.columns = std::move(part->columns);
        budget.release(redshift_routine_usage(old_headers));
        first = false;
      } else {
        if (part->columns.size() != output.columns.size()) return malformed();
        for (std::size_t i = 0; i < output.columns.size(); ++i) {
          const auto& incoming = part->columns[i]; auto& retained = output.columns[i];
          if (incoming.name != retained.name || !incoming.normalized_type || !retained.normalized_type) return malformed();
          const auto& from = *incoming.normalized_type; auto& to = *retained.normalized_type;
          if (from.type != to.type || from.known != to.known || from.decimal_digits != to.decimal_digits ||
              from.declared_type != to.declared_type || from.declared_column_size != to.declared_column_size) return malformed();
          to.column_size = from.column_size == 0 || to.column_size == 0 ? 0 : std::max(to.column_size, from.column_size);
        }
        QueryResult duplicate_headers; duplicate_headers.columns = std::move(part->columns);
        budget.release(redshift_routine_usage(duplicate_headers));
      }
      for (auto& row : part->rows) output.rows.push_back(std::move(row));
    }
    std::stable_sort(output.rows.begin(), output.rows.end(), [tables](const ResultRow& a, const ResultRow& b) {
      if (tables && tables->mode == TablesCatalogRequest::Mode::Tables)
        return std::tie(a[3], a[0], a[1], a[2]) < std::tie(b[3], b[0], b[1], b[2]);
      if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2])
        return std::tie(a[0], a[1], a[2]) < std::tie(b[0], b[1], b[2]);
      if (!tables) {
        std::int32_t left = 0, right = 0;
        const auto& l = *a[16]; const auto& r = *b[16];
        std::from_chars(l.data(), l.data() + l.size(), left);
        std::from_chars(r.data(), r.data() + r.size(), right);
        return left < right;
      }
      return a < b;
    });
  }
  if (expired()) return timeout();
  return BackendResult<QueryResult>{std::move(output), {SessionState::Idle, SessionDisposition::Reusable}};
}

BackendResult<QueryResult> PgDatabaseConnection::execute_routine_catalog(
    const CatalogRequest& request, rs::util::Deadline deadline) {
  const auto native = std::optional<CatalogRequest>{request};
  // Always selected, including refusals: never fall through to PostgreSQL SQL.
  const bool columns=std::holds_alternative<ProcedureColumnsCatalogRequest>(*native);
  const auto unavailable=[this] {
    return local_backend_error(LocalFailure::Unsupported,"Modern routine discovery is unavailable",
        BackendOperation::ExecuteCatalog,session_state());
  };
  if(catalog_mode()!=RedshiftCatalogMode::Show || session_state()!=SessionState::Idle)return unavailable();
  const auto capability=get_parameter("show_discovery");std::uint32_t version=0;
  const auto parsed=std::from_chars(capability.data(),capability.data()+capability.size(),version);
  if(capability.empty() || capability.size()>10 || parsed.ec!=std::errc{} ||
      parsed.ptr!=capability.data()+capability.size() || version<4)return unavailable();
  const auto expired=[deadline]{return rs::util::Clock::now()>=deadline;};
  const auto timeout=[] {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),"Routine discovery deadline expired"};
    error.operation=BackendOperation::ExecuteCatalog;return BackendResult<QueryResult>{std::move(error)};
  };
  if(expired())return timeout();
  RedshiftRoutineBudget budget{catalog_response_limits(),catalog_result_limits()};
  const auto valid_names=std::visit([](const auto& names) {
    using T=std::decay_t<decltype(names)>;
    if constexpr(std::is_same_v<T,ProceduresCatalogRequest> || std::is_same_v<T,ProcedureColumnsCatalogRequest>) {
      const auto valid=[](const auto& value) {
        if(!value)return true;
        if(value->find('\0')!=std::string::npos || !rs::util::utf8_code_point_count(*value))return false;
        return true;
      };
      if(!valid(names.catalog) || !valid(names.schema) || !valid(names.procedure))return false;
      const auto pattern=[](const auto& value,CatalogNameMatch match) {
        if(!value || match!=CatalogNameMatch::Existing)return true;
        bool escape=false;
        for(char ch:*value){if(escape)escape=false;else if(ch=='\\')escape=true;}
        return !escape;
      };
      if(!pattern(names.schema,names.name_matches.schema) || !pattern(names.procedure,names.name_matches.object))return false;
      if constexpr(std::is_same_v<T,ProcedureColumnsCatalogRequest>)
        return valid(names.column) && pattern(names.column,names.name_matches.member);
      return true;
    }
    return false;
  },*native);
  if(!valid_names)return local_backend_error(LocalFailure::InvalidInput,"Invalid routine catalog arguments",
      BackendOperation::ExecuteCatalog,session_state());
  const auto schema_bytes=std::visit([](const auto& names)->std::size_t {
    using T=std::decay_t<decltype(names)>;
    if constexpr(std::is_same_v<T,ProceduresCatalogRequest> || std::is_same_v<T,ProcedureColumnsCatalogRequest>)
      return names.schema?names.schema->size():0;
    return 0;
  },*native);
  if(schema_bytes>catalog_result_limits().max_column_name_bytes ||
      !budget.retain({0,0,schema_bytes,0,schema_bytes}))return redshift_routine_limit();
  const auto schema=redshift_routine_schema(*native);
  // The empty-schema path is an explicit no-match, not a search-path default.
  if(schema && schema->empty())return redshift_routine_result("","",{},columns,budget);
  const auto catalog=std::visit([](const auto& names)->std::optional<std::string> {
    using T=std::decay_t<decltype(names)>;
    if constexpr(std::is_same_v<T,ProceduresCatalogRequest> || std::is_same_v<T,ProcedureColumnsCatalogRequest>)return names.catalog;
    return std::nullopt;
  },*native);
  if (catalog && catalog->empty()) return redshift_routine_result("", schema?std::string_view{*schema}:std::string_view{}, {}, columns, budget);
  std::vector<std::string> databases;
  if (catalog) {
    if (!budget.name(*catalog) || !budget.retain({0, 0, catalog->size(), 0, catalog->size()})) return redshift_routine_limit();
    databases.push_back(*catalog);
  } else {
    if (!budget.exchanges_available(schema?3:2)) return redshift_routine_limit();
    auto scope = redshift_catalog_database_scope(catalog_database_scope(),
        get_parameter_status("datashare_enabled"), get_parameter_status("external_database"));
    if (!scope) {
      BackendError error{scope.error(), scope.error_message()};
      error.error_class = BackendErrorClass::InvalidMetadata; error.operation = BackendOperation::ExecuteCatalog;
      error.disposition = SessionDisposition::Retire; return error;
    }
    if (!budget.exchange()) return redshift_routine_limit();
    auto identity = execute_query(*scope == CatalogDatabaseScope::CurrentDatabase ?
        "SELECT current_database() AS database_name" : "SHOW DATABASES;", deadline);
    if (!identity) return identity.backend_error();
    if (expired()) return timeout();
    if (identity.session_snapshot().state != SessionState::Idle ||
        identity.session_snapshot().disposition != SessionDisposition::Reusable) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError), "Invalid routine identity session state"};
      error.error_class = BackendErrorClass::InvalidMetadata; error.operation = BackendOperation::ExecuteCatalog;
      error.session_state = identity.session_snapshot().state; error.disposition = SessionDisposition::Retire; return error;
    }
    const auto usage = redshift_routine_usage(*identity);
    if (!budget.retain(usage)) return redshift_routine_limit();
    if (*scope == CatalogDatabaseScope::CurrentDatabase) {
      auto database = redshift_schema_database(std::move(identity));
      if (!database) return database.backend_error();
      databases.push_back(std::move(*database));
    } else {
      auto selected = normalize_redshift_databases(std::move(identity));
      if (!selected) return selected.backend_error();
      databases = std::move(*selected);
    }
    for (const auto& database : databases)
      if (!budget.name(database) || !budget.retain({0, 0, database.size(), 0, database.size()})) return redshift_routine_limit();
    budget.release(usage);
  }
  auto combined = redshift_routine_result("", "", {}, columns, budget);
  if (!combined) return combined.backend_error();
  bool first = true;
  for (const auto& database_name : databases) {
    if (expired()) return timeout();
    RedshiftRoutineUsage selected_usage{};
    {
      RedshiftRoutineSchemas selected;
      if(!schema) {
        if(!budget.exchange())return redshift_routine_limit();
        RedshiftRoutineUsage raw_usage{};bool raw_held=false;
        auto discovered=[&]() -> BackendResult<RedshiftRoutineSchemas> {
          auto response=execute_query(redshift_show_schemas_command(database_name),deadline);
          if(!response)return response.backend_error();
          if(expired())return timeout().backend_error();
          if(response.session_snapshot().state!=SessionState::Idle ||
              response.session_snapshot().disposition!=SessionDisposition::Reusable) {
            BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
                "Invalid routine schema discovery session state"};
            error.error_class=BackendErrorClass::InvalidMetadata;error.operation=BackendOperation::ExecuteCatalog;
            error.session_state=response.session_snapshot().state;error.disposition=SessionDisposition::Retire;return error;
          }
          raw_usage=redshift_routine_schema_usage(*response);
          if(!budget.retain(raw_usage))return redshift_routine_limit();
          raw_held=true;
          return normalize_redshift_routine_schemas(database_name,*native,response,budget,deadline);
        }(); // Owning raw response destroyed before its admission is released.
        if(raw_held)budget.release(raw_usage);
        if(!discovered)return discovered.backend_error();
        selected=std::move(*discovered);selected_usage=selected.retained;
      }
      const auto schemas=schema?std::span<const std::string>{&*schema,1}:
          std::span<const std::string>{selected.names};
      if(schemas.size()>std::numeric_limits<std::size_t>::max()/2 ||
          !budget.exchanges_available(schemas.size()*2))return redshift_routine_limit();
      for(std::size_t schema_index=0;schema_index<schemas.size();++schema_index) {
        if(expired())return timeout();
        const auto& selected_schema=schemas[schema_index];
        BackendResult<std::string> database{database_name, {SessionState::Idle, SessionDisposition::Reusable}};
        if(!budget.exchanges_available(2))return redshift_routine_limit();
        RedshiftRoutineUsage routine_storage{};
        {
          std::vector<RedshiftRoutine> routines;
          for(bool function:{false,true}) {
            if(expired())return timeout();
            if(!budget.exchange())return redshift_routine_limit();
            auto response=execute_query(redshift_show_routines_command(*database,selected_schema,function),deadline);
            if(!response)return response.backend_error();
            if(expired())return timeout();
            if(response.session_snapshot().state!=SessionState::Idle ||
                response.session_snapshot().disposition!=SessionDisposition::Reusable) {
              BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),"Invalid routine discovery session state"};
              error.error_class=BackendErrorClass::InvalidMetadata;error.operation=BackendOperation::ExecuteCatalog;
              error.session_state=response.session_snapshot().state;error.disposition=SessionDisposition::Retire;return error;
            }
            const auto usage=redshift_routine_usage(*response);
            if(!budget.retain(usage))return redshift_routine_limit();
            auto normalized=normalize_redshift_routines(*database,selected_schema,function,*native,std::move(response),budget,deadline);
            budget.release(usage);
            if(!normalized)return normalized.backend_error();
            if(expired())return timeout();
            for(auto& routine:*normalized)routines.push_back(std::move(routine));
          }
          std::sort(routines.begin(),routines.end(),[](const auto& a,const auto& b) {
            return std::tie(a.name,a.function,a.signature)<std::tie(b.name,b.function,b.signature);
          });
          auto result=redshift_routine_result(*database,selected_schema,routines,columns,budget);
          if(!result)return result.backend_error();
          if(columns) {
            const auto remaining=(schemas.size()-schema_index-1)*2;
            if(routines.size()>std::numeric_limits<std::size_t>::max()-remaining ||
                !budget.exchanges_available(routines.size()+remaining))return redshift_routine_limit();
            const auto& requested=std::get<ProcedureColumnsCatalogRequest>(*native);
            for(const auto& routine:routines) {
              if(expired())return timeout();
              if(!budget.exchange())return redshift_routine_limit();
              auto response=execute_query(redshift_show_parameters_command(*database,selected_schema,routine),deadline);
              if(!response)return response.backend_error();
              if(expired())return timeout();
              if(response.session_snapshot().state!=SessionState::Idle ||
                  response.session_snapshot().disposition!=SessionDisposition::Reusable) {
                BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),"Invalid routine parameter session state"};
                error.error_class=BackendErrorClass::InvalidMetadata;error.operation=BackendOperation::ExecuteCatalog;
                error.session_state=response.session_snapshot().state;error.disposition=SessionDisposition::Retire;return error;
              }
              const auto usage=redshift_routine_usage(*response);
              if(!budget.retain(usage))return redshift_routine_limit();
              auto parameters=normalize_redshift_parameters(*database,selected_schema,routine,requested,std::move(response),budget,deadline);
              budget.release(usage);
              if(!parameters)return parameters.backend_error();
              if(expired())return timeout();
              // Only the first described header set remains owning; all rows stay
              // charged while moved into the single unpublished result.
              QueryResult headers;headers.columns=std::move(parameters->columns);
              budget.release(redshift_routine_usage(headers));
              for(auto& row:parameters->rows)result->rows.push_back(std::move(row));
            }
            sort_redshift_parameters(*result);
          }
          if (first) {
            QueryResult original_headers; original_headers.columns = std::move(combined->columns);
            combined->columns = std::move(result->columns);
            budget.release(redshift_routine_usage(original_headers));
            first = false;
          } else {
            if (result->columns.size() != combined->columns.size()) return redshift_routine_limit();
            for (std::size_t i = 0; i < combined->columns.size(); ++i) {
              auto& to = *combined->columns[i].normalized_type;
              const auto& from = *result->columns[i].normalized_type;
              if (from.type != to.type || from.known != to.known || from.decimal_digits != to.decimal_digits)
                return redshift_routine_limit();
              to.column_size = std::max(to.column_size, from.column_size);
            }
            QueryResult duplicate_headers; duplicate_headers.columns = std::move(result->columns);
            budget.release(redshift_routine_usage(duplicate_headers));
          }
          for (auto& row : result->rows) combined->rows.push_back(std::move(row));
          routine_storage=redshift_routine_storage_usage(routines);
        } // Per-schema routine identities are destroyed; owning output remains.
        budget.release(routine_storage);
      }
    } // Selected owning schema identities are destroyed before release.
    budget.release(selected_usage);
  }
  if (columns) sort_redshift_parameters(*combined);
  else std::stable_sort(combined->rows.begin(), combined->rows.end());
  if (expired()) return timeout();
  return combined;
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
  auto native = native_catalog_request(request);
  if (!native) return local_backend_error(LocalFailure::InvalidInput,
      "Invalid catalog name semantics", BackendOperation::ExecuteCatalog, session_state());
  const auto semantic_request = *native;
  const bool identifier_show = catalog_profile_ == PgCatalogProfile::Redshift &&
      (std::holds_alternative<TablesCatalogRequest>(*native) ||
       std::holds_alternative<ColumnsCatalogRequest>(*native)) &&
      !std::visit([](const auto& value) { return value.name_matches.existing(); }, *native);
  if (identifier_show) {
    // Private LIKE carrier only. Original semantic tags and caller-owned names remain intact.
    native = redshift_identifier_show_request(*native);
    if (!native) return local_backend_error(LocalFailure::InvalidInput,
        "Invalid catalog name semantics", BackendOperation::ExecuteCatalog, session_state());
    const auto* table = std::get_if<TablesCatalogRequest>(&*native);
    const auto* column = std::get_if<ColumnsCatalogRequest>(&*native);
    if (catalog_mode() != RedshiftCatalogMode::Show || session_state() != SessionState::Idle ||
        (table && !redshift_table_schema_is_literal(*table)) ||
        (column && !redshift_column_names_are_literal(*column)))
      return local_backend_error(LocalFailure::Unsupported,
          "Modern exact Redshift catalog discovery is unavailable",
          BackendOperation::ExecuteCatalog, session_state());
  }
  if (catalog_profile_ == PgCatalogProfile::Redshift) {
    const auto* scoped_tables = std::get_if<TablesCatalogRequest>(&*native);
    const auto* scoped_columns = std::get_if<ColumnsCatalogRequest>(&*native);
    if ((scoped_tables && scoped_tables->mode != TablesCatalogRequest::Mode::TableTypes) || scoped_columns) {
      auto scope = redshift_catalog_database_scope(catalog_database_scope(),
          get_parameter_status("datashare_enabled"), get_parameter_status("external_database"));
      if (!scope) {
        BackendError error{scope.error(), scope.error_message()};
        error.operation = BackendOperation::ExecuteCatalog;
        error.error_class = BackendErrorClass::InvalidMetadata;
        error.disposition = SessionDisposition::Retire;
        return error;
      }
      if (catalog_mode() == RedshiftCatalogMode::Show && scoped_columns &&
          redshift_columns_schema_wide(*scoped_columns))
        return execute_database_discovery(*native, semantic_request, *scope, deadline);
      if (catalog_mode() == RedshiftCatalogMode::Show &&
          (*scope == CatalogDatabaseScope::VisibleDatabases &&
            ((scoped_tables && (scoped_tables->mode == TablesCatalogRequest::Mode::Catalogs ||
                                scoped_tables->mode == TablesCatalogRequest::Mode::Schemas ||
                                redshift_table_schema_is_literal(*scoped_tables))) ||
             (scoped_columns && redshift_column_names_are_literal(*scoped_columns)))))
        return execute_database_discovery(*native, semantic_request, *scope, deadline);
    }
  }
  if (catalog_profile_ == PgCatalogProfile::Redshift &&
      (std::holds_alternative<ProceduresCatalogRequest>(*native) ||
       std::holds_alternative<ProcedureColumnsCatalogRequest>(*native)))
    return execute_routine_catalog(*native, deadline);
  const auto* tables = std::get_if<TablesCatalogRequest>(&*native);
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
    auto normalized = normalize_redshift_tables(*database,*schema,*tables,std::move(response),catalog_detailed_table_types());
    if (expired()) return timeout();
    return normalized;
  }
  const auto* columns=std::get_if<ColumnsCatalogRequest>(&*native);
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
  if (catalog_profile_ == PgCatalogProfile::Redshift) {
    if (const auto* foreign = std::get_if<ForeignKeysCatalogRequest>(&*native)) {
      const auto unavailable = [this] {
        return local_backend_error(LocalFailure::Unsupported,
            "Modern exact Redshift foreign-key discovery is unavailable",
            BackendOperation::ExecuteCatalog, session_state());
      };
      if (catalog_mode() != RedshiftCatalogMode::Show || session_state() != SessionState::Idle)
        return unavailable();
      const bool primary = foreign->primary_table.has_value();
      const bool imported = foreign->foreign_table.has_value();
      const auto valid_name = [](const std::optional<std::string>& value) {
        return !value || (value->find('\0') == std::string::npos &&
            rs::util::utf8_code_point_count(*value).has_value());
      };
      if ((primary && (!valid_name(foreign->primary_catalog) || !valid_name(foreign->primary_schema) ||
                       !valid_name(foreign->primary_table))) ||
          (imported && (!valid_name(foreign->foreign_catalog) || !valid_name(foreign->foreign_schema) ||
                        !valid_name(foreign->foreign_table))))
        return local_backend_error(LocalFailure::InvalidInput, "Invalid foreign-key catalog name",
            BackendOperation::ExecuteCatalog, session_state());
      const auto table = [](const auto& database, const auto& schema, const auto& name)
          -> std::optional<RedshiftForeignKeyTable> {
        if (!database || !schema || !name) return std::nullopt;
        return RedshiftForeignKeyTable{*database, *schema, *name};
      };
      const auto direction = primary && imported ? RedshiftForeignKeyDirection::Both :
          (imported ? RedshiftForeignKeyDirection::Imported : RedshiftForeignKeyDirection::Exported);
      auto plan = redshift_foreign_key_plan(direction,
          primary ? table(foreign->primary_catalog, foreign->primary_schema, foreign->primary_table) : std::nullopt,
          imported ? table(foreign->foreign_catalog, foreign->foreign_schema, foreign->foreign_table) : std::nullopt);
      if (!plan) return unavailable();
      const auto capability = get_parameter("show_discovery");
      std::uint32_t version = 0;
      const auto parsed = std::from_chars(capability.data(), capability.data()+capability.size(), version);
      if (capability.empty() || capability.size()>10 ||
          capability.find_first_not_of("0123456789") != std::string::npos ||
          parsed.ec != std::errc{} || parsed.ptr != capability.data()+capability.size() || version<4)
        return unavailable();
      const auto expired = [deadline] { return rs::util::Clock::now() >= deadline; };
      const auto timeout = [] {
        BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),
            "Foreign-key discovery deadline expired"};
        error.operation = BackendOperation::ExecuteCatalog;
        return BackendResult<QueryResult>{std::move(error)};
      };
      if (expired()) return timeout();
      const auto& target = imported ? *plan->foreign : *plan->primary;
      const std::vector<QueryParameter> parameters{
          {target.database, QueryParameterType::Unspecified},
          {target.schema, QueryParameterType::Unspecified},
          {target.table, QueryParameterType::Unspecified}};
      auto response = execute_prepared(imported ?
          "SHOW CONSTRAINTS FOREIGN KEYS FROM TABLE ?.?.?;" :
          "SHOW CONSTRAINTS FOREIGN KEYS EXPORTED FROM TABLE ?.?.?;", parameters, deadline);
      if (!response) return response.backend_error();
      if (expired()) return timeout();
      auto normalized = normalize_redshift_foreign_keys(*plan, std::move(response));
      if (!normalized) return normalized.backend_error();
      // Sorting is execution-route policy only; the pure normalizer keeps native order.
      const std::size_t start = imported ? 0 : 4;
      std::stable_sort(normalized->rows.begin(), normalized->rows.end(),
          [start](const ResultRow& left, const ResultRow& right) {
            for (std::size_t field = start; field < start+3; ++field) {
              if (*left[field] != *right[field]) return *left[field] < *right[field];
            }
            unsigned left_sequence = 0, right_sequence = 0;
            const auto& l = *left[8]; const auto& r = *right[8];
            std::from_chars(l.data(), l.data()+l.size(), left_sequence);
            std::from_chars(r.data(), r.data()+r.size(), right_sequence);
            return left_sequence < right_sequence;
          });
      if (expired()) return timeout();
      return normalized;
    }
  }
  const auto* keys = std::get_if<PrimaryKeysCatalogRequest>(&*native);
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
  // Pure known-type lookup retains the post-READY publication claim above.
  // Only catalog discovery suspends claims; this owner-only guard holds no
  // endpoint mutex across query construction, transport, normalization or throws.
  const auto cancellation = cancellation_resolver_endpoint();
  if (cancellation && !cancellation->enter_resolver()) {
    // Preserve the already owning unknown entries without fabricating type
    // knowledge or sending discovery after a local claim. The adapter retains
    // the original completion status and suppresses that claimed target cursor.
    return resolved;
  }
  struct ResolverPhase {
    std::shared_ptr<SessionCancellationWire> endpoint;
    ~ResolverPhase() { if (endpoint) endpoint->leave_resolver(); }
  } phase{cancellation};
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
