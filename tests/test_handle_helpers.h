#pragma once

#include "odbc/odbc_handles.h"

#include <memory>

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
