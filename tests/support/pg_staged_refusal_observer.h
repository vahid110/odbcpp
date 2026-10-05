#pragma once

#include "core/transport/i_transport.h"
#include "core/transport/start_tls_transport.h"
#include "core/transport/tls_configurable_transport.h"
#include <array>
#include <memory>

namespace rs::tests::support {

// Synchronous, single-thread test support. No asynchronous transport facet.
class PgStagedRefusalObserver final : public core::transport::ITransport,
    public core::transport::IStartTlsTransport,
    public core::transport::ITlsConfigurableTransport {
public:
  enum class State { FreshUnarmed, Armed, Sealed, Fault };
  enum class Fault { None, Lifecycle, Peer, Deadline, Transport, Progress,
                     Bounds, Frame, Phase, Incomplete };
  enum class Completion { None, DescriptionIdle, ErrorIdle };
  enum class Evidence { TestObservationOnly };
  struct Summary {
    Evidence label{Evidence::TestObservationOnly};
    bool verified{};
    Fault fault{Fault::None};
    Completion completion{Completion::None};
    std::size_t sent_bytes{}, received_bytes{}, sent_frames{}, received_frames{};
    std::size_t parses{}, describes{}, syncs{}, parameter_count{};
    bool ready_idle{};
  };
  static constexpr std::size_t max_bytes = 16384, max_frames = 32;
  static util::Result<std::unique_ptr<PgStagedRefusalObserver>> create(
      std::unique_ptr<core::transport::ITransport> delegate);
  PgStagedRefusalObserver(const PgStagedRefusalObserver&) = delete;
  PgStagedRefusalObserver& operator=(const PgStagedRefusalObserver&) = delete;
  PgStagedRefusalObserver(PgStagedRefusalObserver&&) = delete;
  PgStagedRefusalObserver& operator=(PgStagedRefusalObserver&&) = delete;

  // Identity/Idle/admission are separately trusted caller prerequisites.
  bool arm(util::Deadline original_deadline) noexcept;
  Summary seal() noexcept;
  State state() const noexcept { return state_; }
  Summary summary() const noexcept { return summary_; }

  util::Result<void> connect(std::string_view, std::uint16_t, util::Deadline) override;
  util::Result<void> connect_plain(std::string_view, std::uint16_t, util::Deadline) override;
  util::Result<void> upgrade_to_tls(std::string_view, util::Deadline) override;
  void set_ca_locations(const std::string&, const std::string&) override;
  bool peer_identity_verified() noexcept override;
  util::Result<core::transport::IOResult> send(std::span<const std::byte>, util::Deadline) override;
  util::Result<core::transport::IOResult> recv(std::span<std::byte>, util::Deadline) override;
  void close() noexcept override;

private:
  explicit PgStagedRefusalObserver(std::unique_ptr<core::transport::ITransport>);
  struct Frame {
    std::array<std::byte,5> header{};
    std::array<std::byte,2> prefix{};
    std::size_t header_used{}, body_left{}, body_seen{};
    std::uint32_t length{};
    char tag{};
    void clear() noexcept { *this = {}; }
    bool empty() const noexcept { return header_used == 0; }
  };
  void fail(Fault) noexcept;
  void phase_call() noexcept;
  void check_deadline(util::Deadline) noexcept;
  void observe(std::span<const std::byte>, bool outgoing) noexcept;
  void feed(std::byte, bool outgoing) noexcept;
  void header_ready(Frame&, bool outgoing) noexcept;
  void complete(Frame&, bool outgoing) noexcept;
  std::unique_ptr<core::transport::ITransport> delegate_;
  core::transport::IStartTlsTransport* tls_{};
  core::transport::ITlsConfigurableTransport* configuration_{};
  State state_{State::FreshUnarmed};
  Summary summary_{};
  util::Deadline deadline_{};
  Frame outgoing_, incoming_;
  std::size_t processed_{};
  unsigned out_phase_{}, in_phase_{};
};
} // namespace rs::tests::support
