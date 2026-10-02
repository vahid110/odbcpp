#pragma once

#include "odbc/odbc_handles.h"

#include <memory>

namespace rs::odbc::detail {
// Test-only operation view. Owns the ODBC handle, never exports a physical
// session/facet, and keeps the original DSN/transport/lifecycle fixture intact.
// Tests must serialize calls and release the view before handle unregistration;
// owning lifetime does not reserve a registry operation or permit concurrent I/O.
struct ODBCBackendTestAccess {
  class View {
   public:
    explicit View(std::shared_ptr<ODBCConnection> connection) : connection_(std::move(connection)) {}
    View(const View&) = delete;
    View& operator=(const View&) = delete;
    View(View&&) noexcept = default;
    View& operator=(View&&) noexcept = default;
    auto execute_query(std::string_view sql, rs::util::Deadline deadline) {
      return connection_->backend_query(sql, deadline);
    }
    auto transaction(rs::core::database::TransactionAction action, rs::util::Deadline deadline) {
      return connection_->backend_transaction(action, deadline);
    }
    bool is_connected() const { return connection_->backend_connected(); }
    bool has_health() const { return connection_->backend_lease_ && bool(*connection_->backend_lease_) && connection_->backend_observation_.has_health_facet; }
    bool has_reset() const { return connection_->backend_lease_ && bool(*connection_->backend_lease_) && connection_->backend_observation_.has_reset_facet; }
    rs::core::database::BackendResult<void> check_health(rs::util::Deadline deadline) {
      return connection_->backend_health(deadline);
    }
    rs::core::database::BackendResult<void> reset_session(rs::util::Deadline deadline) {
      return connection_->backend_reset(deadline);
    }
   private:
    std::shared_ptr<ODBCConnection> connection_;
  };
  static View view(std::shared_ptr<ODBCConnection> connection) { return View{std::move(connection)}; }
};
} // namespace rs::odbc::detail

namespace odbcpp::test {

inline SQLHSTMT make_statement(SQLHDBC connection_handle) {
  auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<
      rs::odbc::ODBCConnection>(connection_handle);
  if (!connection) return SQL_NULL_HSTMT;
  auto statement = std::make_unique<rs::odbc::ODBCStatement>(connection);
  const auto handle = reinterpret_cast<SQLHSTMT>(statement.get());
  rs::odbc::HandleRegistry::instance().register_handle(
      handle, std::move(statement), connection_handle);
  return handle;
}

inline SQLHDESC make_descriptor(SQLHDBC connection_handle) {
  auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<
      rs::odbc::ODBCConnection>(connection_handle);
  if (!connection) return SQL_NULL_HDESC;
  auto descriptor = std::make_unique<rs::odbc::ODBCDescriptor>(
      connection.get());
  const auto handle = reinterpret_cast<SQLHDESC>(descriptor.get());
  rs::odbc::HandleRegistry::instance().register_handle(
      handle, std::move(descriptor), connection_handle);
  return handle;
}

}  // namespace odbcpp::test
