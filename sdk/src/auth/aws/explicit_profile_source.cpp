#include "odbcpp/auth/aws/explicit_profile_source.h"
#include <array>
#include <new>
#include <stdexcept>

namespace rs::core::auth::aws::profile {
namespace {
bool public_atom(std::string_view s, std::size_t limit = 256) noexcept {
  if (s.empty() || s.size() > limit) return false;
  for (unsigned char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
  }
  return s != "." && s != "..";
}
bool path(std::string_view s) noexcept {
  if (s.size() < 2 || s.size() > 4096 || s.front() != '/' || s.back() == '/') return false;
  s.remove_prefix(1);
  while (!s.empty()) {
    auto end = s.find('/'); auto part = s.substr(0, end);
    if (!public_atom(part)) return false;
    if (end == std::string_view::npos) break;
    s.remove_prefix(end + 1);
  }
  return true;
}
std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}
bool policy(std::span<const std::byte> bytes, std::string_view profile) noexcept {
  if (bytes.empty() || bytes.size() > 16 * 1024) return false;
  std::string_view input{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
  for (unsigned char c : input) {
    if (c != '\n' && c != '\r' && c != '\t' && (c < 32 || c > 126)) return false;
  }
  bool section = false; unsigned lines = 0; std::array<bool,3> seen{};
  while (!input.empty()) {
    if (++lines > 64) return false;
    const auto end = input.find('\n'); auto line = input.substr(0, end);
    if (end == std::string_view::npos) input = {}; else input.remove_prefix(end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.find('\r') != std::string_view::npos || line.size() > 4096) return false;
    line = trim(line); if (line.empty()) continue;
    if (line.front() == '[') {
      if (section || line.size() != profile.size() + 2 || line.back() != ']' ||
          line.substr(1, line.size()-2) != profile) return false;
      section = true; continue;
    }
    if (!section) return false;
    const auto equals = line.find('='); if (equals == std::string_view::npos) return false;
    auto key = trim(line.substr(0, equals)); auto value = trim(line.substr(equals+1));
    std::size_t index;
    if (key == "aws_access_key_id") index = 0;
    else if (key == "aws_secret_access_key") index = 1;
    else if (key == "aws_session_token") index = 2;
    else return false;
    if (seen[index] || value.empty() || value.size() > 2048) return false;
    // Values are opaque visible ASCII; no comments, quoting or substitutions.
    for (unsigned char c : value) { if (c < 33 || c > 126 || c == '#' || c == ';') return false; }
    seen[index] = true;
  }
  return section && seen[0] && seen[1];
}
}
std::string_view safe_message(Failure) noexcept { return "Explicit profile source refused"; }
SourceGeneration::SourceGeneration(std::string generation)
    : thread_(std::this_thread::get_id()), generation_(std::move(generation)) {}
bool SourceGeneration::retire() noexcept {
  if (thread_ != std::this_thread::get_id()) return false;
  retired_ = true; return true;
}
FrozenCredentials::FrozenCredentials(SourceSpec source, CredentialParts parts)
    : source_(std::move(source)), parts_(std::move(parts)), thread_(std::this_thread::get_id()) {}
SourceSpec FrozenCredentials::move_source(FrozenCredentials& other) {
  if (other.consuming_ || other.thread_ != std::this_thread::get_id())
    throw std::logic_error("Frozen credential ownership unavailable");
  return std::move(other.source_);
}
FrozenCredentials::~FrozenCredentials() { if (consuming_) std::terminate(); }
FrozenCredentials::FrozenCredentials(FrozenCredentials&& other)
    : source_(move_source(other)), parts_(std::move(other.parts_)),
      thread_(other.thread_), valid_(other.valid_) { other.valid_ = false; }
ExplicitProfileSource::ExplicitProfileSource(SourceSpec source, Request request,
    std::shared_ptr<SourceGeneration> generation, Reader reader, Loader loader)
    : source_(std::move(source)), request_(std::move(request)), generation_(std::move(generation)),
      reader_(std::move(reader)), loader_(std::move(loader)), thread_(std::this_thread::get_id()) {}
ExplicitProfileSource::~ExplicitProfileSource() { if (active_) std::terminate(); }
Result<std::unique_ptr<ExplicitProfileSource>> ExplicitProfileSource::create(
    SourceSpec spec, StagingProjection projection, SecretBytes&& input, Request request,
    std::shared_ptr<SourceGeneration> generation, Reader reader, Loader loader) {
  SecretBytes owned{std::move(input)}; // before every validation/refusal
  if (!public_atom(spec.profile) || !public_atom(spec.source_identity) ||
      !public_atom(spec.generation) || !public_atom(spec.file_identity) ||
      !path(spec.credentials_path) || !path(spec.config_path) ||
      spec.credentials_path == spec.config_path || !reader || !loader ||
      request.binding().invariant_error()) return Failure::InvalidSpec;
  if (projection.kind != StagingProjection::Kind::Regular || projection.directory_mode != 0700 ||
      projection.file_mode != 0600 || projection.links != 1 || projection.alias ||
      projection.file_identity != spec.file_identity) return Failure::InvalidProjection;
  if (!generation || generation->thread_ != std::this_thread::get_id() ||
      generation->generation_ != spec.generation ||
      request.binding().source().generation != spec.generation ||
      request.binding().source().identity != spec.source_identity) return Failure::SourceMismatch;
  if (generation->retired_) return Failure::SourceRetired;
  bool valid = false;
  owned.with_bytes([&](auto bytes){ valid = policy(bytes, spec.profile); });
  if (!valid) return Failure::InvalidPolicy;
  try {
    return std::unique_ptr<ExplicitProfileSource>{new ExplicitProfileSource{
        std::move(spec), std::move(request), std::move(generation), std::move(reader), std::move(loader)}};
  } catch (const std::bad_alloc&) { return Failure::AllocationFailed; }
}
Failure ExplicitProfileSource::latch(Failure failure) noexcept {
  if (!first_) first_ = failure;
  return *first_;
}
std::optional<Failure> ExplicitProfileSource::checkpoint(const Cancellation* cancelled) noexcept {
  if (first_) return first_;
  try {
    auto sample = reader_();
    if (first_) return first_; // reader reentry cannot authorize load
    const auto* now = std::get_if<rs::util::Deadline>(&sample);
    if (!now || *now == rs::util::Deadline::min() || *now == rs::util::Deadline::max())
      return latch(Failure::ReadFailed);
    if (highwater_ && *now < *highwater_) return latch(Failure::ClockRollback);
    highwater_ = *now;
    if (generation_->retired_) return latch(Failure::SourceRetired);
    const bool stopped = cancelled && cancelled->stop_requested();
    if (first_) return first_;
    if (generation_->retired_) return latch(Failure::SourceRetired);
    if (stopped) return latch(Failure::Cancelled);
    if (*now >= request_.deadline()) return latch(Failure::DeadlineElapsed);
    return {};
  } catch (...) { return latch(Failure::ReadFailed); }
}
Result<FrozenCredentials> ExplicitProfileSource::load(const Cancellation* cancelled) {
  if (thread_ != std::this_thread::get_id()) return Failure::WrongThread;
  if (active_) return latch(Failure::Reentrant);
  if (first_) return *first_;
  if (consumed_) return Failure::Consumed;
  consumed_ = true; active_ = true;
  struct Guard { bool& active; ~Guard(){ active=false; } } guard{active_};
  if (auto failed = checkpoint(cancelled)) return *failed;
  try {
    auto loaded = loader_(source_, request_);
    if (auto failed = checkpoint(cancelled)) return *failed;
    auto* parts = std::get_if<CredentialParts>(&loaded);
    if (!parts) return latch(Failure::LoadFailed);
    if (parts->access_key.empty()) return latch(Failure::MissingAccessKey);
    if (parts->secret_key.empty()) return latch(Failure::MissingSecretKey);
    // Reject opaque embedded control/NUL in loaded values without copying/logging.
    bool valid = true;
    const auto check = [&](const SecretBytes& secret) {
      secret.with_bytes([&](auto bytes){
        for (auto byte : bytes) { const auto c=std::to_integer<unsigned>(byte); if(c<33 || c>126) valid=false; }
      });
    };
    check(parts->access_key); check(parts->secret_key); check(parts->session_token);
    if (!valid) return latch(Failure::InvalidCredentials);
    // Copy potentially allocating public binding before consuming secret parts.
    auto source = source_;
    return FrozenCredentials{std::move(source), std::move(*parts)};
  } catch (const std::bad_alloc&) { return latch(Failure::AllocationFailed); }
    catch (...) { return latch(Failure::LoadFailed); }
}
} // namespace rs::core::auth::aws::profile
