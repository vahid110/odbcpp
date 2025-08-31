#pragma once
#include <system_error>
#include <string>
#include <type_traits>

namespace rs::util {

// Error codes for database operations
enum class DbErrorCode {
  Success = 0,
  ConnectionFailed,
  AuthenticationFailed,
  QueryFailed,
  Timeout,
  NetworkError,
  TLSError,
  InvalidParameter,
  NotConnected,
  ProtocolError
};

// Error category for database errors
class DbErrorCategory : public std::error_category {
public:
  const char* name() const noexcept override { return "database"; }
  
  std::string message(int ev) const override {
    switch (static_cast<DbErrorCode>(ev)) {
      case DbErrorCode::Success: return "Success";
      case DbErrorCode::ConnectionFailed: return "Connection failed";
      case DbErrorCode::AuthenticationFailed: return "Authentication failed";
      case DbErrorCode::QueryFailed: return "Query failed";
      case DbErrorCode::Timeout: return "Operation timed out";
      case DbErrorCode::NetworkError: return "Network error";
      case DbErrorCode::TLSError: return "TLS/SSL error";
      case DbErrorCode::InvalidParameter: return "Invalid parameter";
      case DbErrorCode::NotConnected: return "Not connected";
      case DbErrorCode::ProtocolError: return "Protocol error";
      default: return "Unknown error";
    }
  }
};

inline const std::error_category& db_error_category() {
  static DbErrorCategory instance;
  return instance;
}

inline std::error_code make_error_code(DbErrorCode e) {
  return {static_cast<int>(e), db_error_category()};
}

// Result type for operations that can fail
template<typename T>
class Result {
private:
  union {
    T value_;
    std::error_code error_;
  };
  bool has_value_;
  std::string error_message_;

public:
  // Success constructor
  Result(T&& value) : value_(std::move(value)), has_value_(true) {}
  Result(const T& value) : value_(value), has_value_(true) {}
  
  // Error constructors
  Result(std::error_code error, std::string message = "") 
    : error_(error), has_value_(false), error_message_(std::move(message)) {}
  
  Result(DbErrorCode error, std::string message = "")
    : error_(make_error_code(error)), has_value_(false), error_message_(std::move(message)) {}
  
  // Move/copy constructors
  Result(Result&& other) noexcept : has_value_(other.has_value_), error_message_(std::move(other.error_message_)) {
    if (has_value_) {
      new(&value_) T(std::move(other.value_));
    } else {
      new(&error_) std::error_code(other.error_);
    }
  }
  
  Result(const Result& other) : has_value_(other.has_value_), error_message_(other.error_message_) {
    if (has_value_) {
      new(&value_) T(other.value_);
    } else {
      new(&error_) std::error_code(other.error_);
    }
  }
  
  ~Result() {
    if (has_value_) {
      value_.~T();
    } else {
      error_.~error_code();
    }
  }
  
  // Assignment operators
  Result& operator=(Result&& other) noexcept {
    if (this != &other) {
      this->~Result();
      new(this) Result(std::move(other));
    }
    return *this;
  }
  
  Result& operator=(const Result& other) {
    if (this != &other) {
      this->~Result();
      new(this) Result(other);
    }
    return *this;
  }
  
  // Accessors
  bool has_value() const noexcept { return has_value_; }
  bool has_error() const noexcept { return !has_value_; }
  
  const T& value() const& { return value_; }
  T& value() & { return value_; }
  T&& value() && { return std::move(value_); }
  
  const std::error_code& error() const { return error_; }
  const std::string& error_message() const { return error_message_; }
  
  // Convenience operators
  explicit operator bool() const noexcept { return has_value_; }
  const T& operator*() const& { return value_; }
  T& operator*() & { return value_; }
  T&& operator*() && { return std::move(value_); }
  
  const T* operator->() const { return &value_; }
  T* operator->() { return &value_; }
};

// Specialization for void
template<>
class Result<void> {
private:
  std::error_code error_;
  std::string error_message_;
  bool has_value_;

public:
  // Success constructor
  Result() : has_value_(true) {}
  
  // Error constructors
  Result(std::error_code error, std::string message = "") 
    : error_(error), error_message_(std::move(message)), has_value_(false) {}
  
  Result(DbErrorCode error, std::string message = "")
    : error_(make_error_code(error)), error_message_(std::move(message)), has_value_(false) {}
  
  // Accessors
  bool has_value() const noexcept { return has_value_; }
  bool has_error() const noexcept { return !has_value_; }
  
  const std::error_code& error() const { return error_; }
  const std::string& error_message() const { return error_message_; }
  
  explicit operator bool() const noexcept { return has_value_; }
};

} // namespace rs::util

// Enable std::error_code for DbErrorCode
namespace std {
template<>
struct is_error_code_enum<rs::util::DbErrorCode> : true_type {};
}