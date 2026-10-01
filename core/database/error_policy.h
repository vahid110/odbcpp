#pragma once

namespace rs::core::database {

// Semantic statement context only; no ODBC statement identifiers cross the
// provider boundary. Native-state mapping is pure diagnostic policy and never
// grants retry or reuse safety, which belongs to the operation's session snapshot.
enum class ErrorContext { Unknown, CreateTable, CreateView, CreateIndex, DropIndex };

} // namespace rs::core::database
