#pragma once

namespace rs::core::transport {

// Strict uses non-blocking sockets and an absolute deadline for every wait.
// SocketTimeout uses the platform's SO_RCVTIMEO/SO_SNDTIMEO behavior.
enum class DeadlineModel {
  Strict,
  SocketTimeout,
};

} // namespace rs::core::transport
