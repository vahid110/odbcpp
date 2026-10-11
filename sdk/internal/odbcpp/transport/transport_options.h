#pragma once

#include "odbcpp/transport/deadline_model.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace rs::core::transport {

enum class TransportMode {
  Auto,
  Async,
  Sync,
};

enum class AsyncEngine {
  Auto,
  IOCP,
  Epoll,
};

// Owning TCP socket policy. Direct transport construction retains disabled
// keepalive; configuration-based factories default to enabled OS tuning.
struct TcpKeepaliveOptions {
  bool enabled{false};
  int idle_seconds{0};
  int interval_seconds{0};
  int probe_count{0};

  friend bool operator==(const TcpKeepaliveOptions&, const TcpKeepaliveOptions&) = default;
};

struct TransportOptions {
  TransportMode mode{TransportMode::Auto};
  std::size_t async_max_inflight{64};
  std::size_t async_queue_depth{256};
  AsyncEngine async_engine{AsyncEngine::Auto};
  DeadlineModel deadline_model{DeadlineModel::Strict};
  TcpKeepaliveOptions tcp_keepalive{true, 0, 0, 0};

  // Applies maps from lowest to highest precedence. Keys and enum values are
  // case-insensitive. Unknown keys are ignored; invalid known values fail.
  static TransportOptions resolve(
      const std::map<std::string, std::string>& driver_parameters,
      const std::map<std::string, std::string>& dsn_parameters,
      const std::map<std::string, std::string>& connection_parameters);
};

std::string_view to_string(TransportMode mode) noexcept;
std::string_view to_string(AsyncEngine engine) noexcept;
std::string_view to_string(DeadlineModel model) noexcept;

} // namespace rs::core::transport
