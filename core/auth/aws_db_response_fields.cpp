#include "aws_db_response_fields.h"
#include <array>
#include <new>

namespace rs::core::auth {
namespace {
struct Schema {
  ResponseShape shape;
  std::array<std::string_view, 4> names;
  IssuerTimestampFormat format;
};
std::optional<Schema> schema(DbCredentialOperation op) noexcept {
  switch (op) {
    case DbCredentialOperation::ServerlessGetCredentials:
      return Schema{ResponseShape::ServerlessObject, {"dbUser", "dbPassword", "expiration", "nextRefreshTime"}, IssuerTimestampFormat::JsonSeconds};
    case DbCredentialOperation::ClusterGetCredentials:
      return Schema{ResponseShape::ClusterResult, {"DbUser", "DbPassword", "Expiration", ""}, IssuerTimestampFormat::UtcIso8601};
    case DbCredentialOperation::ClusterGetCredentialsWithIam:
      return Schema{ResponseShape::ClusterWithIamResult, {"DbUser", "DbPassword", "Expiration", "NextRefreshTime"}, IssuerTimestampFormat::UtcIso8601};
  }
  return std::nullopt;
}
bool valid_shape(ResponseShape shape) noexcept {
  return shape == ResponseShape::ServerlessObject || shape == ResponseShape::ClusterResult || shape == ResponseShape::ClusterWithIamResult;
}
std::size_t atom_size(const FieldAtom& atom) noexcept {
  if (const auto* text = std::get_if<TextBytes>(&atom)) return text->bytes.size();
  if (const auto* number = std::get_if<NumberLexeme>(&atom)) return number->bytes.size();
  return 0;
}
// Matches current core policy: valid UTF8, ASCII C0/DEL refusal only. No claim
// of all Unicode control/confusable filtering, normalization or AWS name validity.
bool user_text(std::string_view s) noexcept {
  if (s.empty() || s.size() > ResponseSnapshot::max_user_bytes) return false;
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
std::string_view view(std::span<const std::byte> bytes) noexcept {
  if (bytes.empty()) return {};
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
}
FieldOccurrence::FieldOccurrence(FieldOccurrence&& other)
    : key_(std::move(other.key_)), atom_(std::move(other.atom_)), owns_(other.owns_) { other.owns_ = false; }
std::optional<FieldError> FieldOccurrence::invariant_error() const noexcept {
  if (!owns_ || key_.empty()) return FieldError{FieldFailure::InvalidFieldName};
  if (key_.size() > ResponseSnapshot::max_key_bytes) return FieldError{FieldFailure::ResourceLimit};
  for (unsigned char c : key_) if (c <= 0x20 || c >= 0x7f) return FieldError{FieldFailure::InvalidFieldName};
  if (atom_.valueless_by_exception()) return FieldError{FieldFailure::WrongType};
  if (std::holds_alternative<NumberLexeme>(atom_) && atom_size(atom_) > ResponseSnapshot::max_lexeme_bytes)
    return FieldError{FieldFailure::ResourceLimit};
  return std::nullopt;
}
FieldOutcome<FieldOccurrence> FieldOccurrence::create(std::string key, FieldAtom&& atom) {
  FieldOccurrence owned{std::move(key), std::move(atom)};
  if (auto error = owned.invariant_error()) return *error;
  return owned;
}
ResponseSnapshot::ResponseSnapshot(ResponseSnapshot&& other) noexcept
    : shape_(other.shape_), occurrences_(std::move(other.occurrences_)), owns_(other.owns_) { other.owns_ = false; }
std::optional<FieldError> ResponseSnapshot::invariant_error() const noexcept {
  if (!owns_ || !valid_shape(shape_)) return FieldError{FieldFailure::InvalidShape};
  if (occurrences_.size() > max_fields) return FieldError{FieldFailure::ResourceLimit};
  std::size_t total = 0;
  for (const auto& field : occurrences_) {
    if (auto error = field.invariant_error()) return error;
    for (auto size : {field.key().size(), atom_size(field.atom())}) {
      if (size > max_aggregate_bytes - total) return FieldError{FieldFailure::ResourceLimit};
      total += size;
    }
  }
  return std::nullopt;
}
FieldOutcome<ResponseSnapshot> ResponseSnapshot::create(ResponseShape shape, std::vector<FieldOccurrence>&& fields) {
  ResponseSnapshot owned{shape, std::move(fields)};
  if (auto error = owned.invariant_error()) return *error;
  return owned;
}
std::string_view FieldError::safe_message() const noexcept {
  switch (failure) {
    case FieldFailure::UnsupportedOperation: return "Credential response operation unsupported";
    case FieldFailure::InvalidShape: return "Credential response shape invalid";
    case FieldFailure::ResourceLimit: return "Credential response exceeds local limits";
    case FieldFailure::InvalidFieldName: return "Credential response field name malformed";
    case FieldFailure::UnknownField: return "Credential response field unsupported";
    case FieldFailure::MissingField: return "Credential response required field missing";
    case FieldFailure::DuplicateField: return "Credential response field duplicated";
    case FieldFailure::WrongType: return "Credential response field type invalid";
    case FieldFailure::InvalidUser: return "Credential response user malformed";
    case FieldFailure::InvalidPassword: return "Credential response password malformed";
    case FieldFailure::InvalidTimestamp: return "Credential response timestamp refused";
    case FieldFailure::Cancelled: return "Credential response extraction cancelled";
    case FieldFailure::AllocationFailed: return "Credential response allocation unavailable";
  }
  return "Credential response field type invalid";
}
FieldOutcome<ExtractedDbFields> extract_db_fields(DbCredentialOperation op, ResponseSnapshot&& snapshot, const Cancellation* cancel) {
  auto owned = std::move(snapshot); // Before every refusal, including cancellation.
  try {
    if (cancel && cancel->stop_requested()) return FieldError{FieldFailure::Cancelled};
    if (!owned.owns_ || !valid_shape(owned.shape_)) return FieldError{FieldFailure::InvalidShape};
    const auto expected = schema(op);
    if (!expected) return FieldError{FieldFailure::UnsupportedOperation};
    if (owned.shape_ != expected->shape) return FieldError{FieldFailure::InvalidShape};
    if (auto error = owned.invariant_error()) return *error;
    std::array<std::size_t, 4> indexes{};
    std::array<unsigned, 4> counts{};
    for (std::size_t i = 0; i < owned.occurrences_.size(); ++i) {
      const auto& key = owned.occurrences_[i].key_;
      std::size_t slot = 0;
      while (slot < 4 && (expected->names[slot].empty() || key != expected->names[slot])) ++slot;
      if (slot == 4) return FieldError{FieldFailure::UnknownField};
      if (++counts[slot] != 1) return FieldError{FieldFailure::DuplicateField};
      indexes[slot] = i;
    }
    for (std::size_t i = 0; i < 3; ++i) if (!counts[i]) return FieldError{FieldFailure::MissingField};
    auto& user = owned.occurrences_[indexes[0]].atom_;
    auto& password = owned.occurrences_[indexes[1]].atom_;
    if (!std::holds_alternative<TextBytes>(user) || !std::holds_alternative<TextBytes>(password)) return FieldError{FieldFailure::WrongType};
    if (atom_size(user) > ResponseSnapshot::max_user_bytes) return FieldError{FieldFailure::ResourceLimit};
    // Validate all timestamp types/role bounds before lexical checks.
    for (std::size_t i = 2; i < 4; ++i) if (counts[i]) {
      const auto& atom = owned.occurrences_[indexes[i]].atom_;
      const bool correct = expected->format == IssuerTimestampFormat::JsonSeconds
          ? std::holds_alternative<NumberLexeme>(atom) : std::holds_alternative<TextBytes>(atom);
      if (!correct) return FieldError{FieldFailure::WrongType};
      if (atom_size(atom) > ResponseSnapshot::max_lexeme_bytes) return FieldError{FieldFailure::ResourceLimit};
    }
    bool good_user = false, good_password = false;
    std::get<TextBytes>(user).bytes.with_bytes([&](auto bytes) { good_user = user_text(view(bytes)); });
    if (!good_user) return FieldError{FieldFailure::InvalidUser};
    std::get<TextBytes>(password).bytes.with_bytes([&](auto bytes) {
      good_password = !bytes.empty();
      for (auto byte : bytes) if (byte == std::byte{0}) good_password = false;
    });
    if (!good_password) return FieldError{FieldFailure::InvalidPassword};
    UtcInstant expiry{};
    for (std::size_t i = 2; i < 4; ++i) if (counts[i]) {
      const auto& atom = owned.occurrences_[indexes[i]].atom_;
      auto parsed = TimestampResult{TimestampFailure::InvalidSyntax};
      if (const auto* number = std::get_if<NumberLexeme>(&atom)) parsed = parse_issuer_timestamp(expected->format, number->bytes);
      else std::get<TextBytes>(atom).bytes.with_bytes([&](auto bytes) { parsed = parse_issuer_timestamp(expected->format, view(bytes)); });
      if (!parsed) return FieldError{FieldFailure::InvalidTimestamp, parsed.failure()};
      if (i == 2) expiry = parsed.value(); // Optional refresh can NEVER replace/extend it.
    }
    std::string principal;
    std::get<TextBytes>(user).bytes.with_bytes([&](auto bytes) { principal.assign(view(bytes)); });
    ExtractedDbFields result{std::move(principal), std::move(std::get<TextBytes>(password).bytes), expiry};
    if (cancel && cancel->stop_requested()) return FieldError{FieldFailure::Cancelled};
    return result;
  } catch (const std::bad_alloc&) { return FieldError{FieldFailure::AllocationFailed}; }
}
} // namespace rs::core::auth
