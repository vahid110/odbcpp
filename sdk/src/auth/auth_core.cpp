#include "odbcpp/auth/auth_core.h"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace rs::core::auth {
namespace detail { struct IssuerIdentity {}; }
namespace {
bool text(std::string_view s) noexcept {
  if (s.empty() || s.size() > 1024) return false;
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x20 || c == 0x7f) return false;
    if (c < 0x80) continue;
    unsigned n = 0, value = 0, minimum = 0;
    if (c >= 0xc2 && c <= 0xdf) { n = 1; value = c & 0x1f; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { n = 2; value = c & 0x0f; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 3; value = c & 7; minimum = 0x10000; }
    else return false;
    if (n > s.size() - i) return false;
    while (n--) {
      const auto d = static_cast<unsigned char>(s[i++]);
      if ((d & 0xc0) != 0x80) return false;
      value = (value << 6) | (d & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
  }
  return true;
}
bool finite(rs::util::Deadline t) noexcept {
  return t != rs::util::Deadline::min() && t != rs::util::Deadline::max();
}
bool required_until(const Request& r, rs::util::Deadline& result) noexcept {
  using Rep = rs::util::Clock::duration::rep;
  static_assert(std::numeric_limits<Rep>::is_integer && std::numeric_limits<Rep>::is_signed);
  const auto base = r.deadline().time_since_epoch().count();
  const auto extra = r.headroom().count();
  if (extra < 0 || base > std::numeric_limits<Rep>::max() - extra) return false;
  result = rs::util::Deadline{rs::util::Clock::duration{base + extra}};
  return finite(result);
}
std::optional<Error> match(const Binding& expected, const Binding& actual) noexcept {
  if (expected.target() != actual.target()) return Error{Reason::TargetMismatch};
  if (expected.source() != actual.source()) return Error{Reason::SourceMismatch};
  if (expected.method() != actual.method()) return Error{Reason::MethodMismatch};
  return {};
}
}
Error::Error(Reason reason) noexcept : reason_(reason), category_(Category::Unavailable) {
  switch (reason) {
    case Reason::UnsupportedMethod: category_ = Category::Unsupported; break;
    case Reason::InvalidBinding: case Reason::InvalidRequest: case Reason::InvalidMaterial:
    case Reason::SecretLimit: category_ = Category::Malformed; break;
    case Reason::TargetMismatch: case Reason::SourceMismatch: case Reason::MethodMismatch:
    case Reason::PrincipalMismatch: case Reason::UntrustedReceipt: case Reason::ConsumedReceipt:
      category_ = Category::Denied; break;
    case Reason::CredentialExpired: case Reason::DeadlineElapsed: category_ = Category::Expired; break;
    case Reason::Cancelled: category_ = Category::Cancelled; break;
    case Reason::ClockRollback: case Reason::IssuerFailed: case Reason::AllocationFailed: break;
    default: reason_ = Reason::IssuerFailed; break;
  }
}
std::string_view Error::safe_message() const noexcept {
  switch (category_) {
    case Category::Unsupported: return "Authentication method unsupported";
    case Category::Denied: return "Authentication admission denied";
    case Category::Malformed: return "Authentication input malformed";
    case Category::Expired: return "Authentication validity expired";
    case Category::Cancelled: return "Authentication cancelled";
    case Category::Unavailable: return "Authentication unavailable";
  }
  return "Authentication unavailable";
}
Outcome<SecretBytes> SecretBytes::create(std::span<const std::byte> bytes) {
  if (bytes.size() > max_bytes) return Error{Reason::SecretLimit};
  try { return SecretBytes{std::vector<std::byte>{bytes.begin(), bytes.end()}}; }
  catch (const std::bad_alloc&) { return Error{Reason::AllocationFailed}; }
}
void SecretBytes::throw_consuming() { throw std::logic_error("Secret consumption already active"); }
SecretBytes::SecretBytes(SecretBytes&& other) {
  if (other.consuming_) throw_consuming();
  bytes_.swap(other.bytes_);
}
SecretBytes& SecretBytes::operator=(SecretBytes&& other) {
  if (consuming_ || other.consuming_) throw_consuming();
  if (this != &other) { wipe(); bytes_.clear(); bytes_.swap(other.bytes_); }
  return *this;
}
void SecretBytes::wipe() noexcept {
  // Best-effort portable overwrite before release. This says nothing about
  // external copies, allocator remnants, swap, SDK buffers or crash dumps.
  volatile std::byte* p = bytes_.data();
  for (std::size_t i = 0; i < bytes_.size(); ++i) p[i] = std::byte{0};
}
SecretBytes::~SecretBytes() { wipe(); }
bool SecretBytes::clear() noexcept {
  if (consuming_) return false;
  wipe(); bytes_.clear(); return true;
}
namespace {
std::optional<Error> binding_policy_error(const TargetInput& target, const SourceInput& source, Method method) noexcept {
  if (target.service != Service::PostgreSql && target.service != Service::Redshift && target.service != Service::Rds)
    return Error{Reason::UnsupportedMethod};
  if (method != Method::OrdinaryPassword && method != Method::TemporaryDatabasePassword)
    return Error{Reason::UnsupportedMethod};
  if (method == Method::TemporaryDatabasePassword && target.service != Service::Redshift)
    return Error{Reason::UnsupportedMethod};
  if ((method == Method::OrdinaryPassword && source.kind != SourceKind::ExternalPassword) ||
      (method == Method::TemporaryDatabasePassword && source.kind != SourceKind::TrustedTemporaryDbIssuer))
    return Error{Reason::UnsupportedMethod};
  if (target.port == 0 || target.port > 65535 || !text(target.endpoint) || !text(target.database) ||
      !text(target.principal) || !text(target.resource_id) || !text(target.tls_identity) ||
      !text(target.trust_policy) || !text(source.identity) || !text(source.generation))
    return Error{Reason::InvalidBinding};
  return {};
}
}
Binding::Binding(Binding&& other) noexcept
    : target_(std::move(other.target_)), source_(std::move(other.source_)), method_(other.method_),
      owns_valid_storage_(other.owns_valid_storage_) {
  other.owns_valid_storage_ = false;
}
std::optional<Error> Binding::invariant_error() const noexcept {
  if (!owns_valid_storage_) return Error{Reason::InvalidBinding};
  return binding_policy_error(target_, source_, method_);
}
Outcome<Binding> Binding::create(TargetInput target, SourceInput source, Method method) {
  if (auto error = binding_policy_error(target, source, method)) return *error;
  return Binding{std::move(target), std::move(source), method};
}
Outcome<Request> Request::create(Binding binding, rs::util::Deadline deadline, rs::util::Clock::duration headroom) {
  if (auto error = binding.invariant_error()) return *error;
  if (!finite(deadline) || headroom < rs::util::Clock::duration::zero() || headroom > std::chrono::hours{24})
    return Error{Reason::InvalidRequest};
  Request r{std::move(binding), deadline, headroom};
  rs::util::Deadline end;
  if (!required_until(r, end)) return Error{Reason::InvalidRequest};
  return r;
}
std::optional<Error> Material::invariant_error() const noexcept {
  if (secret_.empty() || secret_.size() > SecretBytes::max_bytes || !text(principal_) || binding_.invariant_error())
    return Error{Reason::InvalidMaterial};
  const bool ordinary = kind_ == MaterialKind::OrdinaryPassword;
  const bool temporary = kind_ == MaterialKind::TemporaryDatabasePassword;
  if (!ordinary && !temporary) return Error{Reason::UnsupportedMethod};
  if ((ordinary && binding_.method() != Method::OrdinaryPassword) ||
      (temporary && binding_.method() != Method::TemporaryDatabasePassword)) return Error{Reason::MethodMismatch};
  if (ordinary && (validity_.kind != Validity::Kind::NoKnownAcquisitionExpiry ||
      validity_.issued_at != rs::util::Deadline{} || validity_.expires_at != rs::util::Deadline{}))
    return Error{Reason::InvalidMaterial};
  if (temporary && (validity_.kind != Validity::Kind::TrustedMonotonicExpiry ||
      !finite(validity_.issued_at) || !finite(validity_.expires_at) || validity_.issued_at >= validity_.expires_at))
    return Error{Reason::InvalidMaterial};
  return {};
}
Outcome<Material> Material::create(Binding binding, MaterialKind kind, std::string principal,
                                 SecretBytes secret, Validity validity) {
  if (auto error = binding.invariant_error()) return *error;
  bool nul = false;
  secret.with_bytes([&](auto bytes) { nul = std::find(bytes.begin(), bytes.end(), std::byte{0}) != bytes.end(); });
  if (nul) return Error{Reason::InvalidMaterial};
  Material candidate{std::move(binding), kind, std::move(principal), std::move(secret), validity};
  if (auto error = candidate.invariant_error()) return *error;
  return candidate;
}
Outcome<std::unique_ptr<Authority>> Authority::create(Binding binding, TrustedIssuer& issuer, MonotonicClock& clock) {
  if (auto error = binding.invariant_error()) return *error;
  try { return std::unique_ptr<Authority>{new Authority{std::move(binding), issuer, clock}}; }
  catch (const std::bad_alloc&) { return Error{Reason::AllocationFailed}; }
}
Authority::Authority(Binding binding, TrustedIssuer& issuer, MonotonicClock& clock)
    : binding_(std::move(binding)), issuer_(issuer), clock_(clock), identity_(std::make_shared<detail::IssuerIdentity>()) {}
std::optional<Error> Authority::sample(rs::util::Deadline& now) noexcept {
  now = clock_.now();
  if (!finite(now) || (high_water_ && now < *high_water_)) return Error{Reason::ClockRollback};
  high_water_ = now;
  return {};
}
std::optional<Error> Authority::check_request(const Request& r, rs::util::Deadline now, const Cancellation* cancel) const noexcept {
  if ((cancel && cancel->stop_requested())) return Error{Reason::Cancelled};
  if (auto error = r.binding().invariant_error()) return *error;
  if (auto error = match(binding_, r.binding())) return error;
  if (now >= r.deadline()) return Error{Reason::DeadlineElapsed};
  return {};
}
std::optional<Error> Authority::check_material(const Request& r, const Material& material, rs::util::Deadline now) const noexcept {
  if (auto error = material.invariant_error()) return *error;
  if (auto error = match(r.binding(), material.binding())) return error;
  if (material.principal() != r.binding().target().principal) return Error{Reason::PrincipalMismatch};
  if ((material.kind() == MaterialKind::OrdinaryPassword) != (r.binding().method() == Method::OrdinaryPassword))
    return Error{Reason::MethodMismatch};
  const auto validity = material.validity();
  if (validity.kind == Validity::Kind::TrustedMonotonicExpiry) {
    if (validity.issued_at > now) return Error{Reason::InvalidMaterial};
    rs::util::Deadline end;
    if (!required_until(r, end)) return Error{Reason::InvalidRequest};
    if (now >= validity.expires_at || end >= validity.expires_at) return Error{Reason::CredentialExpired};
  }
  return {};
}
Outcome<Receipt> Authority::acquire(const Request& request, const Cancellation* cancel) {
  rs::util::Deadline now;
  if (auto error = sample(now)) return *error;
  if (auto error = check_request(request, now, cancel)) return *error;
  try {
    auto result = issuer_.acquire(request, cancel);
    if (auto error = sample(now)) return *error;
    if (auto error = check_request(request, now, cancel)) return *error;
    if (!result) return result.error();
    if (auto error = check_material(request, result.value(), now)) return *error;
    Receipt receipt{identity_, request, std::move(result).value()};
    // Construction/copying is also within the original budget; no receipt may
    // be published if the clock/cancellation changed during that local work.
    if (auto error = sample(now)) return *error;
    if (auto error = check_request(request, now, cancel)) return *error;
    if (auto error = check_material(request, *receipt.material_, now)) return *error;
    if ((cancel && cancel->stop_requested())) return Error{Reason::Cancelled};
    return receipt;
  } catch (const std::bad_alloc&) { return Error{Reason::AllocationFailed}; }
  catch (...) { return Error{Reason::IssuerFailed}; }
}
Outcome<Material> Authority::take(Receipt&& receipt, const Cancellation* cancel) {
  if (!receipt.material_) return Error{Reason::ConsumedReceipt};
  const auto issuer = receipt.issuer_.lock();
  receipt.issuer_.reset(); // Every take attempt is single-use, including denial.
  auto discard = [&](Error error) -> Outcome<Material> { receipt.material_.reset(); return error; };
  if (!issuer || issuer != identity_) return discard(Error{Reason::UntrustedReceipt});
  rs::util::Deadline now;
  if (auto error = sample(now)) return discard(*error);
  if (auto error = check_request(receipt.request_, now, cancel)) return discard(*error);
  if (auto error = check_material(receipt.request_, *receipt.material_, now)) return discard(*error);
  try {
    Material material = std::move(*receipt.material_);
    receipt.material_.reset();
    if ((cancel && cancel->stop_requested())) return Error{Reason::Cancelled};
    return material;
  } catch (...) { return discard(Error{Reason::IssuerFailed}); }
}
} // namespace rs::core::auth
