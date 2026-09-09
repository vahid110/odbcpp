#pragma once

#include "odbc_handles.h"

#include <chrono>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace rs::odbc::detail {

inline thread_local unsigned int api_call_depth = 0;

class ApiCallScope {
public:
  ApiCallScope() noexcept : outermost_(api_call_depth++ == 0) {}
  ~ApiCallScope() { --api_call_depth; }

  bool outermost() const noexcept { return outermost_; }

private:
  bool outermost_;
};

inline void record_unexpected_exception(SQLHANDLE diagnostic_handle) noexcept {
  if (!diagnostic_handle) return;
  try {
    auto handle = HandleRegistry::instance().get_handle(diagnostic_handle);
    if (handle) {
      handle->set_error(SQLSTATE_GENERAL_ERROR,
                        "Unexpected internal driver exception");
    }
  } catch (...) {
    // Returning an ODBC error is still safe if diagnostics cannot be allocated.
  }
}

inline void record_return_code(SQLHANDLE diagnostic_handle,
                               SQLRETURN return_code) noexcept {
  if (!diagnostic_handle) return;
  try {
    auto handle = HandleRegistry::instance().get_handle(diagnostic_handle);
    if (handle) handle->set_last_return_code(return_code);
  } catch (...) {
  }
}

inline void log_api_result(
    SQLHANDLE diagnostic_handle, std::string_view operation,
    SQLRETURN return_code,
    std::chrono::steady_clock::time_point started) noexcept {
  if (operation.empty() ||
      (return_code != SQL_ERROR && return_code != SQL_SUCCESS_WITH_INFO)) {
    return;
  }
  try {
    auto connection = HandleRegistry::instance().get_connection_for_handle(
        diagnostic_handle);
    const auto level = return_code == SQL_ERROR
        ? rs::core::logging::LogLevel::Error
        : rs::core::logging::LogLevel::Warn;
    if (!connection || !connection->logging_enabled(level)) return;

    auto handle = HandleRegistry::instance().get_handle(diagnostic_handle);
    const auto diagnostic = handle
        ? handle->get_diagnostic_record(1) : std::nullopt;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    connection->log(
        level,
        return_code == SQL_ERROR ? "odbc_api_error" : "odbc_api_warning",
        return_code == SQL_ERROR ? "ODBC API call failed"
                                 : "ODBC API call completed with a warning",
        {{"operation", std::string(operation)},
         {"return_code", std::to_string(return_code)},
         {"sqlstate", diagnostic ? diagnostic->sqlstate : ""},
         {"native_error", std::to_string(
                              diagnostic ? diagnostic->native_error : 0)},
         {"duration_us", std::to_string(elapsed)}});
  } catch (...) {
    // Observability must not alter the ODBC result.
  }
}

template <typename Callback>
SQLRETURN invoke_c_api_with_handles(
    SQLHANDLE diagnostic_handle,
    std::initializer_list<SQLHANDLE> operation_handles,
    std::string_view operation_name,
    Callback&& callback) noexcept {
  ApiCallScope call_scope;
  const auto started = std::chrono::steady_clock::now();
  try {
    auto operation =
        HandleRegistry::instance().lock_handles(operation_handles);
    try {
      const auto result = static_cast<SQLRETURN>(
          std::forward<Callback>(callback)());
      record_return_code(diagnostic_handle, result);
      if (call_scope.outermost()) {
        log_api_result(diagnostic_handle, operation_name, result, started);
      }
      return result;
    } catch (...) {
      record_unexpected_exception(diagnostic_handle);
      record_return_code(diagnostic_handle, SQL_ERROR);
      if (call_scope.outermost()) {
        log_api_result(
            diagnostic_handle, operation_name, SQL_ERROR, started);
      }
      return SQL_ERROR;
    }
  } catch (...) {
    record_unexpected_exception(diagnostic_handle);
    record_return_code(diagnostic_handle, SQL_ERROR);
    if (call_scope.outermost()) {
      log_api_result(diagnostic_handle, operation_name, SQL_ERROR, started);
    }
    return SQL_ERROR;
  }
}

template <typename Callback>
SQLRETURN invoke_c_api_with_handles(
    SQLHANDLE diagnostic_handle,
    std::initializer_list<SQLHANDLE> operation_handles,
    Callback&& callback) noexcept {
  return invoke_c_api_with_handles(
      diagnostic_handle, operation_handles, {},
      std::forward<Callback>(callback));
}

template <typename Callback>
SQLRETURN invoke_c_api(SQLHANDLE diagnostic_handle,
                       std::string_view operation_name,
                       Callback&& callback) noexcept {
  return invoke_c_api_with_handles(
      diagnostic_handle, {diagnostic_handle}, operation_name,
      std::forward<Callback>(callback));
}

template <typename Callback>
SQLRETURN invoke_c_api(SQLHANDLE diagnostic_handle,
                       Callback&& callback) noexcept {
  return invoke_c_api(
      diagnostic_handle, {}, std::forward<Callback>(callback));
}

}  // namespace rs::odbc::detail
