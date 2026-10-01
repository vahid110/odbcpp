#pragma once

#include "core/util/result.h"
#include <optional>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace rs::core::database {

enum class BackendOperation {
  Unknown, ExecuteDirect, ExecutePrepared, Describe, Transaction,
  BeginTransaction, CommitTransaction, RollbackTransaction, SetTransactionIsolation
};
enum class SessionState { Disconnected, Idle, Transaction, FailedTransaction, Unknown };
enum class SessionDisposition { Reusable, ResetRequired, Retire };
enum class BackendErrorClass {
  Unknown, Connection, Authentication, Server, Timeout, Transport, Tls,
  InvalidInput, NotConnected, Protocol, Unsupported
};

inline BackendErrorClass classify_backend_error(const std::error_code& code) {
  using rs::util::DbErrorCode;
  if (code == std::errc::timed_out) return BackendErrorClass::Timeout;
  if (code.category() != rs::util::db_error_category()) return BackendErrorClass::Unknown;
  switch (static_cast<DbErrorCode>(code.value())) {
    case DbErrorCode::ConnectionFailed: return BackendErrorClass::Connection;
    case DbErrorCode::AuthenticationFailed: return BackendErrorClass::Authentication;
    case DbErrorCode::QueryFailed: return BackendErrorClass::Server;
    case DbErrorCode::Timeout: return BackendErrorClass::Timeout;
    case DbErrorCode::NetworkError: return BackendErrorClass::Transport;
    case DbErrorCode::TLSError: return BackendErrorClass::Tls;
    case DbErrorCode::InvalidParameter: return BackendErrorClass::InvalidInput;
    case DbErrorCode::NotConnected: return BackendErrorClass::NotConnected;
    case DbErrorCode::ProtocolError: return BackendErrorClass::Protocol;
    case DbErrorCode::UnsupportedFeature: return BackendErrorClass::Unsupported;
    default: return BackendErrorClass::Unknown;
  }
}

// Owning operation failure. Native details never determine session reuse.
// Reusable means protocol-ready for this owner, not reset for a different borrower.
// Retry is absent unless a backend can prove it; no automatic replay is implied.
struct BackendError {
  std::error_code code;
  std::string message;
  BackendErrorClass error_class;
  std::optional<std::string> native_state;
  std::optional<std::int64_t> native_code;
  BackendOperation operation{BackendOperation::Unknown};
  SessionState session_state{SessionState::Unknown};
  SessionDisposition disposition{SessionDisposition::Retire};
  std::optional<bool> retry_safe;

  BackendError(std::error_code error, std::string text)
      : code(error), message(std::move(text)), error_class(classify_backend_error(error)) {}
};

// Only local validation/unsupported failures perform no I/O and preserve state.
enum class LocalFailure { InvalidInput, Unsupported };
inline BackendError local_backend_error(LocalFailure kind,
    std::string message, BackendOperation operation, SessionState state) {
  const auto code = kind == LocalFailure::InvalidInput ? rs::util::DbErrorCode::InvalidParameter :
      rs::util::DbErrorCode::UnsupportedFeature;
  BackendError error{rs::util::make_error_code(code), std::move(message)};
  error.operation = operation;
  error.session_state = state;
  error.disposition = state == SessionState::Disconnected ? SessionDisposition::Retire :
      state == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired;
  return error;
}

// Internal migration contract. The std::error_code/message accessors preserve
// existing diagnostic callers; backend_error() carries the owning SDK detail.
// Accessing the wrong alternative throws rather than reading an inactive union.
template<class T>
class BackendResult {
 public:
  BackendResult(T value) : data_(std::move(value)) {}
  BackendResult(BackendError error) : data_(std::move(error)) {}
  BackendResult(std::error_code code, std::string message = {})
      : data_(BackendError{code, std::move(message)}) {}
  BackendResult(rs::util::DbErrorCode code, std::string message = {})
      : BackendResult(rs::util::make_error_code(code), std::move(message)) {}
  bool has_value() const noexcept { return std::holds_alternative<T>(data_); }
  bool has_error() const noexcept { return !has_value(); }
  explicit operator bool() const noexcept { return has_value(); }
  T& value() & { return std::get<T>(data_); }
  const T& value() const& { return std::get<T>(data_); }
  T&& value() && { return std::get<T>(std::move(data_)); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(*this).value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }
  BackendError& backend_error() { return std::get<BackendError>(data_); }
  const BackendError& backend_error() const { return std::get<BackendError>(data_); }
  const std::error_code& error() const { return backend_error().code; }
  const std::string& error_message() const { return backend_error().message; }
 private:
  std::variant<T, BackendError> data_;
};

template<>
class BackendResult<void> {
 public:
  BackendResult() = default;
  BackendResult(BackendError error) : error_(std::move(error)) {}
  BackendResult(std::error_code code, std::string message = {})
      : error_(BackendError{code, std::move(message)}) {}
  BackendResult(rs::util::DbErrorCode code, std::string message = {})
      : BackendResult(rs::util::make_error_code(code), std::move(message)) {}
  bool has_value() const noexcept { return !error_; }
  bool has_error() const noexcept { return error_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }
  BackendError& backend_error() { return error_.value(); }
  const BackendError& backend_error() const { return error_.value(); }
  const std::error_code& error() const { return backend_error().code; }
  const std::string& error_message() const { return backend_error().message; }
 private:
  std::optional<BackendError> error_;
};
}  // namespace rs::core::database
