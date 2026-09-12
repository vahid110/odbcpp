#pragma once
#include "odbc_types.h"
#include "core/database/i_database_connection.h"
#include "core/database/connection_pool.h"
#include "core/util/driver_logging.h"
#include "core/util/result.h"
#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <map>
#include <mutex>
#include <optional>

namespace rs::odbc {

class HandleRegistry;

// Diagnostic record for ODBC error handling
struct DiagnosticRecord {
  std::string sqlstate;
  SQLINTEGER native_error;
  std::string message_text;
  std::string class_origin;
  std::string subclass_origin;
  std::string connection_name;
  std::string server_name;
  SQLLEN row_number;
  SQLINTEGER column_number;

  static bool has_odbc_origin(std::string_view state) {
    if (state.starts_with("IM")) return true;
    static constexpr std::array<std::string_view, 31> odbc_subclasses{
        "01S00", "01S01", "01S02", "01S06", "01S07", "07S01",
        "08S01", "21S01", "21S02", "25S01", "25S02", "25S03",
        "42S01", "42S02", "42S11", "42S12", "42S21", "42S22",
        "HY095", "HY097", "HY098", "HY099", "HY100", "HY101",
        "HY105", "HY107", "HY109", "HY110", "HY111", "HYT00",
        "HYT01"};
    for (const auto subclass : odbc_subclasses) {
      if (state == subclass) return true;
    }
    return false;
  }
  
  DiagnosticRecord(const std::string& state = SQLSTATE_SUCCESS, 
                   SQLINTEGER native = 0,
                   const std::string& message = "")
    : sqlstate(state), native_error(native), message_text(message),
      class_origin(std::string_view(state).starts_with("IM")
                       ? "ODBC 3.0" : "ISO 9075"),
      subclass_origin(has_odbc_origin(state) ? "ODBC 3.0" : "ISO 9075"),
      row_number(SQL_NO_ROW_NUMBER), column_number(SQL_NO_COLUMN_NUMBER) {}
};

struct DiagnosticHeader {
  SQLLEN cursor_row_count{0};
  SQLLEN row_count{0};
  std::string dynamic_function;
  SQLINTEGER dynamic_function_code{SQL_DIAG_UNKNOWN_STATEMENT};
};

// Base ODBC handle with diagnostic-record storage.
class ODBCHandle {
public:
  explicit ODBCHandle(HandleType type) : type_(type) {}
  virtual ~ODBCHandle() = default;
  
  HandleType get_type() const { return type_; }
  
  // Diagnostic record management
  void clear_diagnostics() {
    std::lock_guard lock(diagnostics_mutex_);
    diagnostic_records_.clear();
    diagnostic_header_ = {};
  }
  
  void add_diagnostic(const std::string& sqlstate, SQLINTEGER native_error, const std::string& message) {
    std::lock_guard lock(diagnostics_mutex_);
    diagnostic_records_.emplace_back(sqlstate, native_error, message);
  }
  
  void set_error(const std::string& sqlstate, const std::string& message, SQLINTEGER native_error = 0) {
    std::lock_guard lock(diagnostics_mutex_);
    diagnostic_records_.clear();
    diagnostic_records_.emplace_back(sqlstate, native_error, message);
  }
  
  // First-record accessors retained for existing internal callers.
  std::string get_sqlstate() const { 
    std::lock_guard lock(diagnostics_mutex_);
    return diagnostic_records_.empty() ? std::string(SQLSTATE_SUCCESS) : diagnostic_records_[0].sqlstate;
  }
  
  std::string get_error_message() const {
    std::lock_guard lock(diagnostics_mutex_);
    return diagnostic_records_.empty() ? std::string() : diagnostic_records_[0].message_text;
  }
  
  // Diagnostic record access
  size_t get_diagnostic_count() const {
    std::lock_guard lock(diagnostics_mutex_);
    return diagnostic_records_.size();
  }
  
  std::optional<DiagnosticRecord> get_diagnostic_record(
      SQLSMALLINT record_number) const {
    std::lock_guard lock(diagnostics_mutex_);
    if (record_number < 1 || record_number > static_cast<SQLSMALLINT>(diagnostic_records_.size())) {
      return std::nullopt;
    }
    return diagnostic_records_[record_number - 1];
  }

  void set_last_return_code(SQLRETURN return_code) {
    std::lock_guard lock(diagnostics_mutex_);
    last_return_code_ = return_code;
  }

  SQLRETURN get_last_return_code() const {
    std::lock_guard lock(diagnostics_mutex_);
    return last_return_code_;
  }

  void set_statement_diagnostic_header(
      SQLLEN cursor_row_count, SQLLEN row_count,
      std::string dynamic_function, SQLINTEGER dynamic_function_code) {
    std::lock_guard lock(diagnostics_mutex_);
    diagnostic_header_.cursor_row_count = cursor_row_count;
    diagnostic_header_.row_count = row_count;
    diagnostic_header_.dynamic_function = std::move(dynamic_function);
    diagnostic_header_.dynamic_function_code = dynamic_function_code;
  }

  DiagnosticHeader get_diagnostic_header() const {
    std::lock_guard lock(diagnostics_mutex_);
    return diagnostic_header_;
  }

private:
  friend class HandleRegistry;

  HandleType type_;
  std::recursive_mutex operation_mutex_;
  mutable std::mutex diagnostics_mutex_;
  std::vector<DiagnosticRecord> diagnostic_records_;
  DiagnosticHeader diagnostic_header_;
  SQLRETURN last_return_code_{SQL_SUCCESS};
};

// Pins and serializes one or more handles within their connection ownership
// domains. Every lease takes locks in registry order so overlapping operations
// cannot deadlock, while independent connections remain concurrent.
class HandleOperationLease {
public:
  HandleOperationLease() = default;
  HandleOperationLease(HandleOperationLease&&) noexcept = default;
  HandleOperationLease& operator=(HandleOperationLease&&) noexcept = default;
  HandleOperationLease(const HandleOperationLease&) = delete;
  HandleOperationLease& operator=(const HandleOperationLease&) = delete;

private:
  friend class HandleRegistry;

  std::vector<std::shared_ptr<ODBCHandle>> handles_;
  std::vector<std::unique_lock<std::recursive_mutex>> locks_;
};

// Environment handle
class ODBCEnvironment : public ODBCHandle {
public:
  ODBCEnvironment() : ODBCHandle(HandleType::Environment) {}
  
  void set_odbc_version(SQLINTEGER version) { odbc_version_ = version; }
  SQLINTEGER get_odbc_version() const { return odbc_version_; }
  bool has_odbc_version() const { return odbc_version_ != 0; }

private:
  SQLINTEGER odbc_version_ = 0;
};

// Connection handle
class ODBCConnection : public ODBCHandle {
public:
  explicit ODBCConnection(ODBCEnvironment* env);
  
  SQLRETURN connect(const std::string& dsn, const std::string& user, const std::string& password);
  SQLRETURN disconnect();
  bool is_connected() const { return connected_; }
  SQLRETURN set_attribute(SQLINTEGER attribute, SQLULEN value);
  SQLRETURN get_attribute(SQLINTEGER attribute, SQLPOINTER value);
  SQLRETURN set_current_catalog(std::string catalog);
  std::string get_current_catalog() const;
  SQLRETURN end_transaction(SQLSMALLINT completion_type);
  rs::util::Result<void> begin_transaction_if_needed(
      rs::util::Deadline deadline);
  void log(rs::core::logging::LogLevel level, std::string_view event,
           std::string_view message,
           std::initializer_list<rs::core::logging::LogField> fields = {})
      const noexcept;
  bool logging_enabled(rs::core::logging::LogLevel level) const noexcept;
  bool logs_queries() const noexcept;
  std::uint64_t connection_id() const noexcept { return connection_id_; }
  
  rs::core::database::IDatabaseConnection* get_db_connection() { return db_conn_.get(); }

private:
  void close_connection();

  std::unique_ptr<rs::core::database::IDatabaseConnection> db_conn_;
  bool connected_ = false;
  SQLUINTEGER login_timeout_seconds_ = 30;
  SQLUINTEGER connection_timeout_seconds_ = 0;
  SQLUINTEGER autocommit_ = SQL_AUTOCOMMIT_ON;
  SQLUINTEGER transaction_isolation_ = SQL_TXN_READ_COMMITTED;
  SQLHWND quiet_mode_ = nullptr;
  bool transaction_active_ = false;
  std::optional<std::string> requested_catalog_;
  std::string current_catalog_;
  std::uint64_t connection_id_{};
  std::shared_ptr<rs::core::logging::DriverLogger> logger_;
  
};

// IRD (Implementation Row Descriptor) - ODBC result column metadata
struct ColumnInfo {
  std::string name;           // SQL_DESC_NAME
  SQLSMALLINT sql_type;       // SQL_DESC_TYPE
  SQLULEN column_size;        // SQL_DESC_LENGTH
  SQLSMALLINT decimal_digits; // SQL_DESC_PRECISION
  SQLSMALLINT nullable;       // SQL_DESC_NULLABLE
};

bool is_character_column_attribute(SQLUSMALLINT field_identifier);

// IPD (Implementation Parameter Descriptor) - ODBC parameter metadata
struct ParameterMetadata {
  SQLSMALLINT sql_type;           // SQL_VARCHAR, SQL_INTEGER, etc.
  SQLULEN column_size;
  SQLSMALLINT decimal_digits;
  SQLSMALLINT nullable;
  std::string name;               // Parameter name (if available)
};

struct DescriptorRecord {
  SQLSMALLINT type{SQL_C_DEFAULT};
  SQLSMALLINT concise_type{SQL_C_DEFAULT};
  SQLSMALLINT datetime_interval_code{0};
  SQLINTEGER datetime_interval_precision{0};
  SQLULEN length{0};
  SQLSMALLINT precision{0};
  SQLSMALLINT scale{0};
  SQLSMALLINT nullable{SQL_NULLABLE_UNKNOWN};
  SQLSMALLINT parameter_type{SQL_PARAM_INPUT};
  SQLPOINTER data_ptr{nullptr};
  SQLLEN* indicator_ptr{nullptr};
  SQLLEN* octet_length_ptr{nullptr};
  SQLLEN octet_length{0};
  std::string name;
  std::string base_column_name;
  std::string base_table_name;
  std::string catalog_name;
  std::string label;
  std::string literal_prefix;
  std::string literal_suffix;
  std::string local_type_name;
  std::string schema_name;
  std::string table_name;
  std::string type_name;
  SQLINTEGER auto_unique_value{SQL_FALSE};
  SQLINTEGER case_sensitive{SQL_FALSE};
  SQLLEN display_size{0};
  SQLSMALLINT fixed_prec_scale{SQL_FALSE};
  SQLINTEGER num_prec_radix{0};
  SQLSMALLINT rowver{SQL_FALSE};
  SQLSMALLINT searchable{SQL_PRED_SEARCHABLE};
  SQLSMALLINT unnamed{SQL_UNNAMED};
  SQLSMALLINT unsigned_attribute{SQL_FALSE};
  SQLSMALLINT updatable{SQL_ATTR_READONLY};
};

enum class DescriptorKind {
  Application,
  ImplementationRow,
  ImplementationParameter,
};

class ODBCDescriptor : public ODBCHandle {
public:
  explicit ODBCDescriptor(ODBCConnection*,
                          bool automatically_allocated = false,
                          DescriptorKind kind = DescriptorKind::Application)
      : ODBCHandle(HandleType::Descriptor),
        automatically_allocated_(automatically_allocated), kind_(kind) {}

  bool is_automatically_allocated() const {
    return automatically_allocated_;
  }
  SQLULEN array_size() const noexcept { return array_size_; }
  SQLUSMALLINT* array_status_ptr() const noexcept {
    return array_status_ptr_;
  }
  SQLLEN* bind_offset_ptr() const noexcept { return bind_offset_ptr_; }
  SQLULEN bind_type() const noexcept { return bind_type_; }
  SQLULEN* rows_processed_ptr() const noexcept {
    return rows_processed_ptr_;
  }
  std::uint64_t revision() const noexcept { return revision_; }
  std::size_t record_count() const noexcept { return records_.size(); }
  const DescriptorRecord* record(std::size_t index) const noexcept {
    return index < records_.size() ? &records_[index] : nullptr;
  }

  SQLRETURN get_field(SQLSMALLINT record_number,
                      SQLSMALLINT field_identifier, SQLPOINTER value,
                      SQLINTEGER buffer_length, SQLINTEGER* string_length);
  SQLRETURN set_field(SQLSMALLINT record_number,
                      SQLSMALLINT field_identifier, SQLPOINTER value,
                      SQLINTEGER buffer_length);
  SQLRETURN get_record(SQLSMALLINT record_number, SQLCHAR* name,
                       SQLSMALLINT buffer_length,
                       SQLSMALLINT* string_length, SQLSMALLINT* type,
                       SQLSMALLINT* subtype, SQLLEN* length,
                       SQLSMALLINT* precision, SQLSMALLINT* scale,
                       SQLSMALLINT* nullable);
  SQLRETURN set_record(SQLSMALLINT record_number, SQLSMALLINT type,
                       SQLSMALLINT subtype, SQLLEN length,
                       SQLSMALLINT precision, SQLSMALLINT scale,
                       SQLPOINTER data, SQLLEN* string_length,
                       SQLLEN* indicator);
  SQLRETURN copy_from(const ODBCDescriptor& source);

private:
  friend class ODBCStatement;
  void replace_records(std::vector<DescriptorRecord> records) {
    records_ = std::move(records);
    ++revision_;
  }

  bool automatically_allocated_{false};
  DescriptorKind kind_{DescriptorKind::Application};
  std::vector<DescriptorRecord> records_;
  SQLULEN array_size_{1};
  SQLUSMALLINT* array_status_ptr_{nullptr};
  SQLLEN* bind_offset_ptr_{nullptr};
  SQLULEN bind_type_{SQL_BIND_BY_COLUMN};
  SQLULEN* rows_processed_ptr_{nullptr};
  std::uint64_t revision_{0};
};

// Statement handle
class ODBCStatement : public ODBCHandle {
public:
  explicit ODBCStatement(std::shared_ptr<ODBCConnection> conn);
  ~ODBCStatement() override;
  
  SQLRETURN execute_direct(const std::string& sql);
  SQLRETURN fetch();
  SQLRETURN more_results();
  SQLRETURN get_data(SQLUSMALLINT col, SQLSMALLINT target_type, 
                     void* buffer, SQLLEN buffer_length, SQLLEN* indicator);
  SQLRETURN set_attribute(SQLINTEGER attribute, SQLPOINTER value);
  SQLRETURN get_attribute(SQLINTEGER attribute, SQLPOINTER value);
  void detach_descriptor(SQLHDESC descriptor) noexcept;
  SQLRETURN close_cursor(bool report_missing_cursor);
  void unbind_columns();
  void reset_parameters();
  
  // Prepared statements
  SQLRETURN prepare(const std::string& sql);
  SQLRETURN execute();
  SQLRETURN num_params(SQLSMALLINT* parameter_count);
  SQLRETURN bind_parameter(SQLUSMALLINT parameter_number, SQLSMALLINT input_output_type,
                          SQLSMALLINT value_type, SQLSMALLINT parameter_type, SQLULEN column_size,
                          SQLSMALLINT decimal_digits, SQLPOINTER parameter_value, SQLLEN buffer_length,
                          SQLLEN* strlen_or_indicator);
  
  // Column binding (ARD)
  SQLRETURN bind_col(SQLUSMALLINT column_number, SQLSMALLINT target_type,
                     SQLPOINTER target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator);
  
  // Metadata functions
  SQLRETURN get_num_result_cols(SQLSMALLINT* column_count);
  SQLRETURN get_type_info(SQLSMALLINT data_type);
  SQLRETURN columns(const std::optional<std::string>& catalog_name,
                    const std::optional<std::string>& schema_name,
                    const std::optional<std::string>& table_name,
                    const std::optional<std::string>& column_name);
  SQLRETURN primary_keys(const std::optional<std::string>& catalog_name,
                         const std::optional<std::string>& schema_name,
                         const std::string& table_name);
  SQLRETURN foreign_keys(
      const std::optional<std::string>& pk_catalog_name,
      const std::optional<std::string>& pk_schema_name,
      const std::optional<std::string>& pk_table_name,
      const std::optional<std::string>& fk_catalog_name,
      const std::optional<std::string>& fk_schema_name,
      const std::optional<std::string>& fk_table_name);
  SQLRETURN statistics(const std::optional<std::string>& catalog_name,
                       const std::optional<std::string>& schema_name,
                       const std::string& table_name,
                       bool unique_only);
  SQLRETURN procedures(const std::optional<std::string>& catalog_name,
                       const std::optional<std::string>& schema_name,
                       const std::optional<std::string>& procedure_name);
  SQLRETURN procedure_columns(
      const std::optional<std::string>& catalog_name,
      const std::optional<std::string>& schema_name,
      const std::optional<std::string>& procedure_name,
      const std::optional<std::string>& column_name);
  SQLRETURN special_columns(
      SQLUSMALLINT identifier_type,
      const std::optional<std::string>& catalog_name,
      const std::optional<std::string>& schema_name,
      const std::string& table_name, bool require_non_nullable);
  SQLRETURN tables(const std::optional<std::string>& catalog_name,
                   const std::optional<std::string>& schema_name,
                   const std::optional<std::string>& table_name,
                   const std::optional<std::string>& table_type);
  SQLRETURN row_count(SQLLEN* row_count);
  SQLRETURN describe_col(SQLUSMALLINT column_number, SQLCHAR* column_name, SQLSMALLINT name_buffer_length,
                        SQLSMALLINT* name_length, SQLSMALLINT* data_type, SQLULEN* column_size,
                        SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable);
  SQLRETURN col_attribute(SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
                         SQLPOINTER character_attribute, SQLSMALLINT buffer_length,
                         SQLSMALLINT* string_length, SQLLEN* numeric_attribute);
  
  // Parameter metadata (IPD)
  SQLRETURN describe_param(SQLUSMALLINT parameter_number, SQLSMALLINT* data_type,
                          SQLULEN* parameter_size, SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable);
  
  bool has_results() const { return !result_rows_.empty(); }
  size_t get_column_count() const { return result_rows_.empty() ? 0 : result_rows_[0].size(); }

private:
  std::shared_ptr<ODBCConnection> conn_;
  rs::core::database::ResultRows result_rows_;
  std::vector<ColumnInfo> column_info_;        // IRD storage
  std::vector<ParameterMetadata> param_metadata_; // IPD storage
  std::size_t get_data_offset_ = 0;
  SQLUSMALLINT get_data_column_ = 0;
  std::vector<rs::core::database::QueryResult> pending_results_;
  std::string prepared_sql_;
  size_t current_row_ = 0;
  bool executed_ = false;
  bool prepared_ = false;
  bool prepared_metadata_available_ = false;
  std::uint64_t prepared_metadata_ipd_revision_{0};
  SQLSMALLINT parameter_count_ = 0;
  SQLLEN affected_rows_ = 0;
  SQLULEN query_timeout_seconds_ = 0;
  SQLULEN max_rows_ = 0;
  SQLHDESC automatic_app_row_descriptor_{SQL_NULL_HDESC};
  SQLHDESC automatic_app_param_descriptor_{SQL_NULL_HDESC};
  SQLHDESC app_row_descriptor_{SQL_NULL_HDESC};
  SQLHDESC app_param_descriptor_{SQL_NULL_HDESC};
  SQLHDESC imp_row_descriptor_{SQL_NULL_HDESC};
  SQLHDESC imp_param_descriptor_{SQL_NULL_HDESC};

  void apply_query_result(rs::core::database::QueryResult result,
                          bool include_parameter_metadata);
  void apply_result_metadata(
      const rs::core::database::QueryResult& result,
      bool include_parameter_metadata);
  SQLRETURN ensure_result_metadata();
  SQLRETURN describe_prepared_metadata();
  void clear_current_result();
  SQLRETURN complete_parameter_set(SQLRETURN result);
  SQLHDESC create_implicit_descriptor(DescriptorKind kind);
  SQLRETURN set_application_descriptor(SQLINTEGER attribute,
                                       SQLHDESC descriptor);
  std::shared_ptr<ODBCDescriptor> descriptor(SQLHDESC handle) const;
};

// Handle registry for validation
class HandleRegistry {
public:
  static HandleRegistry& instance();
  
  void register_handle(SQLHANDLE handle, std::unique_ptr<ODBCHandle> obj,
                       SQLHANDLE parent = SQL_NULL_HANDLE);
  void unregister_handle(SQLHANDLE handle);
  void unregister_children(SQLHANDLE parent);
  bool has_children(SQLHANDLE parent);
  std::vector<SQLHANDLE> child_handles(SQLHANDLE parent, HandleType type);
  std::shared_ptr<ODBCHandle> get_handle(SQLHANDLE handle);
  std::shared_ptr<ODBCConnection> get_connection_for_handle(SQLHANDLE handle);
  void detach_descriptor_from_statements(SQLHDESC descriptor);
  HandleOperationLease lock_handles(
      std::initializer_list<SQLHANDLE> handles);
  
  template<typename T>
  std::shared_ptr<T> get_handle_as(SQLHANDLE handle) {
    return std::dynamic_pointer_cast<T>(get_handle(handle));
  }

private:
  struct Entry {
    std::shared_ptr<ODBCHandle> object;
    SQLHANDLE parent{SQL_NULL_HANDLE};
  };

  void collect_subtree_locked(
      SQLHANDLE handle, std::vector<std::shared_ptr<ODBCHandle>>& removed);

  std::map<SQLHANDLE, Entry> handles_;
  std::mutex mutex_;
};

} // namespace rs::odbc
