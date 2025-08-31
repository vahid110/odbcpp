#pragma once
#include "result.h"
#include "errors.h"

namespace rs::util {

// Helper to convert Result<T> to exceptions for backward compatibility
template<typename T>
T unwrap_or_throw(Result<T>&& result) {
  if (result.has_value()) {
    return std::move(*result);
  }
  
  const auto& error = result.error();
  const auto& message = result.error_message();
  
  switch (static_cast<DbErrorCode>(error.value())) {
    case DbErrorCode::Timeout:
      throw TimeoutError(message.empty() ? error.message() : message);
    case DbErrorCode::TLSError:
      throw TLSError(message.empty() ? error.message() : message);
    case DbErrorCode::NetworkError:
    case DbErrorCode::ConnectionFailed:
    case DbErrorCode::ProtocolError:
      throw IOError(message.empty() ? error.message() : message);
    default:
      throw std::runtime_error(message.empty() ? error.message() : message);
  }
}

// Specialization for void
inline void unwrap_or_throw(Result<void>&& result) {
  if (result.has_error()) {
    const auto& error = result.error();
    const auto& message = result.error_message();
    
    switch (static_cast<DbErrorCode>(error.value())) {
      case DbErrorCode::Timeout:
        throw TimeoutError(message.empty() ? error.message() : message);
      case DbErrorCode::TLSError:
        throw TLSError(message.empty() ? error.message() : message);
      case DbErrorCode::NetworkError:
      case DbErrorCode::ConnectionFailed:
      case DbErrorCode::ProtocolError:
        throw IOError(message.empty() ? error.message() : message);
      default:
        throw std::runtime_error(message.empty() ? error.message() : message);
    }
  }
}

// Helper to convert exceptions to Result<T>
template<typename F>
auto try_catch(F&& func) -> Result<decltype(func())> {
  try {
    if constexpr (std::is_void_v<decltype(func())>) {
      func();
      return Result<void>{};
    } else {
      return Result<decltype(func())>{func()};
    }
  } catch (const TimeoutError& e) {
    return Result<decltype(func())>{DbErrorCode::Timeout, e.what()};
  } catch (const TLSError& e) {
    return Result<decltype(func())>{DbErrorCode::TLSError, e.what()};
  } catch (const IOError& e) {
    return Result<decltype(func())>{DbErrorCode::NetworkError, e.what()};
  } catch (const std::exception& e) {
    return Result<decltype(func())>{DbErrorCode::ProtocolError, e.what()};
  }
}

} // namespace rs::util