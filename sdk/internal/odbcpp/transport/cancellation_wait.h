#pragma once

#include "odbcpp/transport/i_transport.h"
#include <algorithm>
#include <atomic>
#include <memory>

namespace rs::core::transport {

// Main transport waits retain their caller deadline. A cancellation request may
// tighten it once; polling this control never touches the main socket or SSL.
class CancellationWait final {
 public:
  bool freeze(rs::util::Deadline original, rs::util::Deadline now) noexcept {
    using Clock = rs::util::Deadline::clock;
    if (now == rs::util::Deadline::min() || now == rs::util::Deadline::max() || now >= original) return false;
    const auto budget = std::chrono::duration_cast<Clock::duration>(std::chrono::seconds{5});
    if (now.time_since_epoch() > Clock::duration::max() - budget) return false;
    const auto value = (std::min)(original, now + budget).time_since_epoch().count();
    auto expected = Clock::duration::max().count();
    return cutoff_.compare_exchange_strong(expected, value, std::memory_order_release, std::memory_order_relaxed);
  }
  rs::util::Deadline effective(rs::util::Deadline original) const noexcept {
    using Clock = rs::util::Deadline::clock;
    return (std::min)(original, rs::util::Deadline{Clock::duration{cutoff_.load(std::memory_order_acquire)}});
  }
  rs::util::Deadline slice(rs::util::Deadline original) const noexcept {
    const auto limit = effective(original);
    const auto now = rs::util::Deadline::clock::now();
    const auto step = std::chrono::duration_cast<rs::util::Deadline::clock::duration>(std::chrono::milliseconds{50});
    if (now.time_since_epoch() > rs::util::Deadline::clock::duration::max() - step) return limit;
    return (std::min)(limit, now + step);
  }
 private:
  std::atomic<rs::util::Deadline::clock::duration::rep> cutoff_{rs::util::Deadline::clock::duration::max().count()};
};

// Optional private Sync carrier. Capture/installation happens on the main owner;
// only the independent peer is accessed from the cancellation thread.
class IServerCancelTransport {
 public:
  virtual ~IServerCancelTransport() = default;
  virtual bool supports_server_cancel() const noexcept = 0;
  virtual void cancellation_wait(std::shared_ptr<CancellationWait>) noexcept = 0;
  virtual std::unique_ptr<ITransport> cancellation_peer() const = 0;
  virtual rs::util::Result<void> connect_cancellation_peer(rs::util::Deadline) = 0;
};
} // namespace rs::core::transport
