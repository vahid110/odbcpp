#pragma once
#include "odbc_types.h"
#include "core/database/i_database_connection.h"
#include "core/database/connection_pool.h"
#include "core/util/driver_logging.h"
#include "core/util/result.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <optional>

namespace rs::odbc {

// Diagnostic record for ODBC error handling
struct DiagnosticRecord {
  std::string sqlstate;
  SQLINTEGER native_error;
  std::string message_text;
  std::string class_origin;
  std::string subclass_origin;
  std::string connection_name;
  std::string server_name;
  
  DiagnosticRecord(const std::string& state = SQLSTATE_SUCCESS, 
                   SQLINTEGER native = 0,
                   const std::string& message = "")
    : sqlstate(state), native_error(native), message_text(message),
      class_origin("ISO 9075"), subclass_origin("ODBCPP 1.0") {}
};

// Base ODBC handle with diagnostic-record storage.
class ODBCHandle {
public:
  explicit ODBCHandle(HandleType type) : type_(type) {}
  virtual ~ODBCHandle() = default;
  
  HandleType get_type() const { return type_; }
  
  // Diagnostic record management
  void clear_diagnostics() {
    diagnostic_records_.clear();
  }
  
  void add_diagnostic(const std::string& sqlstate, SQLINTEGER native_error, const std::string& message) {
    diagnostic_records_.emplace_back(sqlstate, native_error, message);
  }
  
  void set_error(const std::string& sqlstate, const std::string& message, SQLINTEGER native_error = 0) {
    clear_diagnostics();
    add_diagnostic(sqlstate, native_error, message);
  }
  
  // First-record accessors retained for existing internal callers.
  std::string get_sqlstate() const { 
    return diagnostic_records_.empty() ? std::string(SQLSTATE_SUCCESS) : diagnostic_records_[0].sqlstate;
  }
  
  std::string get_error_message() const {
    return diagnostic_records_.empty() ? std::string() : diagnostic_records_[0].message_text;
  }
  
  // Diagnostic record access
  size_t get_diagnostic_count() const { return diagnostic_records_.size(); }
  
  const DiagnosticRecord* get_diagnostic_record(SQLSMALLINT record_number) const {
    if (record_number < 1 || record_number > static_cast<SQLSMALLINT>(diagnostic_records_.size())) {
      return nullptr;
    }
    return &diagnostic_records_[record_number - 1];
  }

private:
  HandleType type_;
  std::vector<DiagnosticRecord> diagnostic_records_;
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
  SQLRETURN get_attribute(SQLINTEGER attribute, SQLUINTEGER* value);
  SQLRETURN end_transaction(SQLSMALLINT completion_type);
  rs::util::Result<void> begin_transaction_if_needed(
      rs::util::Deadline deadline);
  void log(rs::core::logging::LogLevel level, std::string_view event,
           std::string_view message,
           std::initializer_list<rs::core::logging::LogField> fields = {})
      const noexcept;
  bool logs_queries() const noexcept;
  std::uint64_t connection_id() const noexcept { return connection_id_; }
  
  rs::core::database::IDatabaseConnection* get_db_connection() { return db_conn_.get(); }

private:
  void close_connection();

  std::unique_ptr<rs::core::database::IDatabaseConnection> db_conn_;
  bool connected_ = false;
  SQLUINTEGER login_timeout_seconds_ = 30;
  SQLUINTEGER autocommit_ = SQL_AUTOCOMMIT_ON;
  SQLUINTEGER transaction_isolation_ = SQL_TXN_READ_COMMITTED;
  bool transaction_active_ = false;
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

// APD (Application Parameter Descriptor) - ODBC parameter binding info
struct ParameterInfo {
  SQLSMALLINT input_output_type{SQL_PARAM_INPUT};
  SQLSMALLINT value_type{SQL_C_DEFAULT};
  SQLSMALLINT parameter_type{SQL_UNKNOWN_TYPE};
  SQLULEN column_size{0};
  SQLSMALLINT decimal_digits{0};
  SQLPOINTER parameter_value{nullptr};
  SQLLEN buffer_length{0};
  SQLLEN* strlen_or_indicator{nullptr};
  bool bound{false};
};

// ARD (Application Row Descriptor) - ODBC result column binding info
struct ColumnBinding {
  SQLSMALLINT target_type;        // SQL_C_CHAR, SQL_C_LONG, etc.
  SQLPOINTER target_value;        // Application buffer
  SQLLEN buffer_length;
  SQLLEN* strlen_or_indicator;
  bool bound = false;
};

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
  SQLULEN length{0};
  SQLSMALLINT precision{0};
  SQLSMALLINT scale{0};
  SQLSMALLINT nullable{SQL_NULLABLE_UNKNOWN};
  SQLPOINTER data_ptr{nullptr};
  SQLLEN* indicator_ptr{nullptr};
  SQLLEN* octet_length_ptr{nullptr};
  SQLLEN octet_length{0};
  std::string name;
};

class ODBCDescriptor : public ODBCHandle {
public:
  explicit ODBCDescriptor(ODBCConnection*,
                          bool automatically_allocated = false)
      : ODBCHandle(HandleType::Descriptor),
        automatically_allocated_(automatically_allocated) {}

  bool is_automatically_allocated() const {
    return automatically_allocated_;
  }

  SQLRETURN get_field(SQLSMALLINT record_number,
                      SQLSMALLINT field_identifier, SQLPOINTER value,
                      SQLINTEGER buffer_length, SQLINTEGER* string_length);
  SQLRETURN set_field(SQLSMALLINT record_number,
                      SQLSMALLINT field_identifier, SQLPOINTER value,
                      SQLINTEGER buffer_length);
  void copy_from(const ODBCDescriptor& source);

private:
  bool automatically_allocated_{false};
  std::vector<DescriptorRecord> records_;
  SQLULEN array_size_{1};
  SQLUSMALLINT* array_status_ptr_{nullptr};
  SQLLEN* bind_offset_ptr_{nullptr};
  SQLULEN bind_type_{SQL_BIND_BY_COLUMN};
  SQLULEN* rows_processed_ptr_{nullptr};
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
  SQLRETURN set_attribute(SQLINTEGER attribute, SQLULEN value);
  SQLRETURN get_attribute(SQLINTEGER attribute, SQLULEN* value);
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
  std::vector<ParameterInfo> parameter_info_;  // APD storage
  std::vector<ColumnBinding> column_bindings_; // ARD storage
  std::vector<ParameterMetadata> param_metadata_; // IPD storage
  std::vector<std::size_t> get_data_offsets_;
  std::vector<rs::core::database::QueryResult> pending_results_;
  std::string prepared_sql_;
  size_t current_row_ = 0;
  bool executed_ = false;
  bool prepared_ = false;
  SQLSMALLINT parameter_count_ = 0;
  SQLLEN affected_rows_ = 0;
  SQLULEN query_timeout_seconds_ = 0;
  SQLULEN max_rows_ = 0;
  SQLHDESC app_row_descriptor_{SQL_NULL_HDESC};
  SQLHDESC app_param_descriptor_{SQL_NULL_HDESC};
  SQLHDESC imp_row_descriptor_{SQL_NULL_HDESC};
  SQLHDESC imp_param_descriptor_{SQL_NULL_HDESC};

  void apply_query_result(rs::core::database::QueryResult result,
                          bool include_parameter_metadata);
  SQLHDESC create_implicit_descriptor();
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
  std::shared_ptr<ODBCHandle> get_handle(SQLHANDLE handle);
  
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
