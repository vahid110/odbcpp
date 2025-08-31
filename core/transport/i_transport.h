#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include "core/util/deadline.h"

namespace rs::core::transport {

struct IOResult { std::size_t n{}; bool eof{false}; };

class ITransport {
public:
  virtual ~ITransport() = default;
  virtual void connect(std::string_view host, uint16_t port,
                       rs::util::Deadline deadline) = 0;
  virtual IOResult send(std::span<const std::byte> buf,
                        rs::util::Deadline deadline) = 0;
  virtual IOResult recv(std::span<std::byte> buf,
                        rs::util::Deadline deadline) = 0;
  virtual void close() noexcept = 0;
};

} // namespace rs::core::transport