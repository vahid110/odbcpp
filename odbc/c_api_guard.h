#pragma once

#include "odbc_handles.h"

#include <initializer_list>
#include <utility>

namespace rs::odbc::detail {

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

template <typename Callback>
SQLRETURN invoke_c_api_with_handles(
    SQLHANDLE diagnostic_handle,
    std::initializer_list<SQLHANDLE> operation_handles,
    Callback&& callback) noexcept {
  try {
    auto operation =
        HandleRegistry::instance().lock_handles(operation_handles);
    try {
      const auto result = static_cast<SQLRETURN>(
          std::forward<Callback>(callback)());
      record_return_code(diagnostic_handle, result);
      return result;
    } catch (...) {
      record_unexpected_exception(diagnostic_handle);
      record_return_code(diagnostic_handle, SQL_ERROR);
      return SQL_ERROR;
    }
  } catch (...) {
    record_unexpected_exception(diagnostic_handle);
    record_return_code(diagnostic_handle, SQL_ERROR);
    return SQL_ERROR;
  }
}

template <typename Callback>
SQLRETURN invoke_c_api(SQLHANDLE diagnostic_handle,
                       Callback&& callback) noexcept {
  return invoke_c_api_with_handles(
      diagnostic_handle, {diagnostic_handle},
      std::forward<Callback>(callback));
}

}  // namespace rs::odbc::detail
