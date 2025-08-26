// transport_deadline.h
#pragma once
#include <chrono>
#include <system_error>

namespace rs::core::transport {

struct Deadline {
  using clock = std::chrono::steady_clock;
  clock::time_point at;

  static Deadline from_now(std::chrono::milliseconds d) {
    return {clock::now() + d};
  }
  static Deadline none() { return {clock::time_point::max()}; }
  bool expired() const { return clock::now() >= at; }
  std::chrono::milliseconds remaining_ms() const {
    if (at == clock::time_point::max()) return std::chrono::milliseconds::max();
    auto now = clock::now();
    if (now >= at) return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(at - now);
  }
};

enum class Errc {
  Ok = 0,
  Timeout,
  Disconnected,
  WantRetry,        // transient (e.g., EAGAIN / WANT_READ/WRITE)
  HandshakeFailed,
  CertVerifyFailed,
  SyscallFailed,
  InvalidArgument,
};

struct Error {
  Errc code{Errc::Ok};
  int  sys_errno{0};     // from errno or WSAGetLastError()
  long ssl_err{0};       // from ERR_get_error()
  const char* where{""}; // "connect", "handshake", "send", "recv"
  const char* detail{""};
};

[[noreturn]] inline void throw_io(const Error& e); // you can implement to map to your existing exceptions

} // namespace rs::core::transport
