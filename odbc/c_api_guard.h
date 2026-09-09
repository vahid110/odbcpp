#pragma once

#include "odbc_handles.h"

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

template <typename Callback>
SQLRETURN invoke_c_api(SQLHANDLE diagnostic_handle,
                       Callback&& callback) noexcept {
  try {
    return std::forward<Callback>(callback)();
  } catch (...) {
    record_unexpected_exception(diagnostic_handle);
    return SQL_ERROR;
  }
}

}  // namespace rs::odbc::detail
