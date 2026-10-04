#pragma once

#include "core/util/deadline.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rs::core::auth {

// Private portable admission boundary. No AWS/GSS/ODBC/Arrow/network types.
// In-process Authority identity is not cryptographic issuer proof. The embedding
// coordinator is responsible for selecting a trusted issuer and monotonic clock.
// All objects are thread-confined; publication is an instantaneous local check,
// not a reservation against later cancellation or permission/credential changes.
enum class Category { Unsupported, Denied, Malformed, Expired, Cancelled, Unavailable };
enum class Reason {
  UnsupportedMethod, InvalidBinding, InvalidRequest, InvalidMaterial, SecretLimit,
  TargetMismatch, SourceMismatch, MethodMismatch, PrincipalMismatch,
  UntrustedReceipt, ConsumedReceipt, CredentialExpired, DeadlineElapsed,
  Cancelled, ClockRollback, IssuerFailed, AllocationFailed
};
class Error final {
 public:
  explicit Error(Reason reason) noexcept;
  Category category() const noexcept { return category_; }
  Reason reason() const noexcept { return reason_; }
  std::string_view safe_message() const noexcept;
 private:
  Reason reason_;
  Category category_;
};
template<class T> class Outcome final {
 public:
  Outcome(T value) : value_(std::move(value)) {}
  Outcome(Error error) : value_(error) {}
  explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
  T& value() & { return std::get<T>(value_); }
  const T& value() const& { return std::get<T>(value_); }
  T&& value() && { return std::get<T>(std::move(value_)); }
  const Error& error() const { return std::get<Error>(value_); }
 private:
  std::variant<T, Error> value_;
};

// Local resource limit, not an AWS password/token maximum. No serializer or
// implicit string/span conversion. Views must not escape the callback. C++ cannot
// prevent a hostile consumer copying bytes. Clear/moves/destruction must not race
// consumption; reentrant clear is refused and reentrant move throws fixed text.
class SecretBytes final {
 public:
  static constexpr std::size_t max_bytes = 65536;
  static Outcome<SecretBytes> create(std::span<const std::byte> bytes);
  SecretBytes(const SecretBytes&) = delete;
  SecretBytes& operator=(const SecretBytes&) = delete;
  SecretBytes(SecretBytes&& other);
  SecretBytes& operator=(SecretBytes&& other);
  ~SecretBytes();
  bool empty() const noexcept { return bytes_.empty(); }
  std::size_t size() const noexcept { return bytes_.size(); }
  bool clear() noexcept;
  template<class Consumer> void with_bytes(Consumer&& consumer) const {
    if (consuming_) throw_consuming();
    consuming_ = true;
    struct Guard { bool& flag; ~Guard() { flag = false; } } guard{consuming_};
    std::invoke(std::forward<Consumer>(consumer), std::span<const std::byte>{bytes_});
  }
 private:
  friend class Material;
  explicit SecretBytes(std::vector<std::byte> bytes) : bytes_(std::move(bytes)) {}
  static void throw_consuming();
  void wipe() noexcept;
  std::vector<std::byte> bytes_;
  mutable bool consuming_{false};
};

enum class Service { PostgreSql, Redshift, Rds };
enum class Method { OrdinaryPassword, TemporaryDatabasePassword };
enum class SourceKind { ExternalPassword, TrustedTemporaryDbIssuer };
struct TargetInput {
  Service service;
  std::string endpoint;
  unsigned port;
  std::string database, principal, resource_id, tls_identity, trust_policy;
  bool operator==(const TargetInput&) const = default;
};
struct SourceInput {
  SourceKind kind;
  std::string identity, generation;
  bool operator==(const SourceInput&) const = default;
};
// TLS identity/trust fields express mandatory verified-peer intent. This core
// does not verify a peer or establish that any TLS handshake occurred.
class Binding final {
 public:
  static Outcome<Binding> create(TargetInput target, SourceInput source, Method method);
  Binding(const Binding&) = default;
  Binding(Binding&& other) noexcept;
  Binding& operator=(const Binding&) = delete;
  Binding& operator=(Binding&&) = delete;
  const TargetInput& target() const noexcept { return target_; }
  const SourceInput& source() const noexcept { return source_; }
  Method method() const noexcept { return method_; }
  std::optional<Error> invariant_error() const noexcept;
 private:
  Binding(TargetInput target, SourceInput source, Method method)
      : target_(std::move(target)), source_(std::move(source)), method_(method) {}
  TargetInput target_;
  SourceInput source_;
  Method method_;
  bool owns_valid_storage_{true}; // Local moved-from state, never issuer proof.
};
class Request final {
 public:
  static Outcome<Request> create(Binding binding, rs::util::Deadline deadline,
                                rs::util::Clock::duration headroom);
  const Binding& binding() const noexcept { return binding_; }
  rs::util::Deadline deadline() const noexcept { return deadline_; }
  rs::util::Clock::duration headroom() const noexcept { return headroom_; }
 private:
  Request(Binding binding, rs::util::Deadline deadline, rs::util::Clock::duration headroom)
      : binding_(std::move(binding)), deadline_(deadline), headroom_(headroom) {}
  Binding binding_;
  rs::util::Deadline deadline_;
  rs::util::Clock::duration headroom_;
};

enum class MaterialKind { OrdinaryPassword, TemporaryDatabasePassword };
struct Validity {
  enum class Kind { NoKnownAcquisitionExpiry, TrustedMonotonicExpiry };
  Kind kind;
  rs::util::Deadline issued_at{}, expires_at{};
  static Validity ordinary() noexcept { return {Kind::NoKnownAcquisitionExpiry, {}, {}}; }
  // Trust comes from explicit issuer injection, not this public value factory.
  // UTC conversion, sampling/uncertainty and server response proof are issuer-owned.
  static Validity monotonic(rs::util::Deadline issued, rs::util::Deadline expires) noexcept {
    return {Kind::TrustedMonotonicExpiry, issued, expires};
  }
};
class Material final {
 public:
  static Outcome<Material> create(Binding binding, MaterialKind kind,
      std::string returned_principal, SecretBytes secret, Validity validity);
  Material(const Material&) = delete;
  Material& operator=(const Material&) = delete;
  Material(Material&&) = default;
  Material& operator=(Material&&) = delete;
  const Binding& binding() const noexcept { return binding_; }
  MaterialKind kind() const noexcept { return kind_; }
  const std::string& principal() const noexcept { return principal_; }
  Validity validity() const noexcept { return validity_; }
  template<class Consumer> void with_secret(Consumer&& consumer) const {
    secret_.with_bytes(std::forward<Consumer>(consumer));
  }
 private:
  friend class Authority;
  std::optional<Error> invariant_error() const noexcept;
  Material(Binding binding, MaterialKind kind, std::string principal, SecretBytes secret, Validity validity)
      : binding_(std::move(binding)), kind_(kind), principal_(std::move(principal)),
        secret_(std::move(secret)), validity_(validity) {}
  Binding binding_;
  MaterialKind kind_;
  std::string principal_;
  SecretBytes secret_;
  Validity validity_;
};
// Pure injected cancellation observation; no threads, timers or OS callbacks.
// Observation must be noexcept; integration owns synchronization/lifetime.
class Cancellation {
 public:
  virtual ~Cancellation() = default;
  virtual bool stop_requested() const noexcept = 0;
};
class TrustedIssuer {
 public:
  virtual ~TrustedIssuer() = default;
  // Exact request/deadline and cancellation are passed unchanged. Implementations
  // must not return raw source diagnostics; Authority catches thrown failures.
  virtual Outcome<Material> acquire(const Request&, const Cancellation*) = 0;
};
class MonotonicClock {
 public:
  virtual ~MonotonicClock() = default;
  virtual rs::util::Deadline now() noexcept = 0;
};
namespace detail { struct IssuerIdentity; }
class Authority;
class Receipt final {
 public:
  Receipt(const Receipt&) = delete;
  Receipt& operator=(const Receipt&) = delete;
  Receipt(Receipt&&) = default;
  Receipt& operator=(Receipt&&) = delete;
 private:
  friend class Authority;
  Receipt(std::weak_ptr<const detail::IssuerIdentity> issuer, Request request, Material material)
      : issuer_(std::move(issuer)), request_(std::move(request)), material_(std::move(material)) {}
  std::weak_ptr<const detail::IssuerIdentity> issuer_;
  Request request_;
  std::optional<Material> material_;
};
class Authority final {
 public:
  // Creation is explicit coordinator trust assignment, not discovery or proof.
  // Injected issuer/clock must outlive this thread-confined authority.
  static Outcome<std::unique_ptr<Authority>> create(Binding binding, TrustedIssuer& issuer, MonotonicClock& clock);
  Authority(const Authority&) = delete;
  Authority& operator=(const Authority&) = delete;
  Authority(Authority&&) = delete;
  Authority& operator=(Authority&&) = delete;
  Outcome<Receipt> acquire(const Request& request, const Cancellation* cancelled = nullptr);
  // Attempt consumes the receipt even on denial. Returned material is an owning
  // handoff; this does not authenticate or authorize subsequent SQL/session reuse.
  Outcome<Material> take(Receipt&& receipt, const Cancellation* cancelled = nullptr);
 private:
  Authority(Binding binding, TrustedIssuer& issuer, MonotonicClock& clock);
  std::optional<Error> sample(rs::util::Deadline& now) noexcept;
  std::optional<Error> check_request(const Request&, rs::util::Deadline, const Cancellation*) const noexcept;
  std::optional<Error> check_material(const Request&, const Material&, rs::util::Deadline) const noexcept;
  Binding binding_;
  TrustedIssuer& issuer_;
  MonotonicClock& clock_;
  std::shared_ptr<const detail::IssuerIdentity> identity_;
  std::optional<rs::util::Deadline> high_water_;
};
} // namespace rs::core::auth
