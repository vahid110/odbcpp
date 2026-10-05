#pragma once
#include "odbcpp/auth/issuer_timestamp.h"
#include <vector>

namespace rs::core::auth {
// Representation tags only. Neither public tag nor snapshot is issuer proof.
enum class DbCredentialOperation { ServerlessGetCredentials, ClusterGetCredentials, ClusterGetCredentialsWithIam };
enum class ResponseShape { ServerlessObject, ClusterResult, ClusterWithIamResult };
struct TextBytes { SecretBytes bytes; };
struct NumberLexeme { std::string bytes; };
struct NullAtom {}; struct BooleanAtom {}; struct ObjectAtom {}; struct ArrayAtom {};
using FieldAtom = std::variant<TextBytes, NumberLexeme, NullAtom, BooleanAtom, ObjectAtom, ArrayAtom>;
enum class FieldFailure {
  UnsupportedOperation, InvalidShape, ResourceLimit, InvalidFieldName, UnknownField,
  MissingField, DuplicateField, WrongType, InvalidUser, InvalidPassword,
  InvalidTimestamp, Cancelled, AllocationFailed
};
struct FieldError {
  FieldFailure failure;
  std::optional<TimestampFailure> timestamp_failure{};
  std::string_view safe_message() const noexcept;
};
template<class T> class FieldOutcome final {
 public:
  FieldOutcome(T value) : value_(std::move(value)) {}
  FieldOutcome(FieldError error) : value_(error) {}
  explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
  T& value() & { return std::get<T>(value_); }
  const T& value() const& { return std::get<T>(value_); }
  T&& value() && { return std::get<T>(std::move(value_)); }
  FieldError error() const { return std::get<FieldError>(value_); }
 private:
  std::variant<T, FieldError> value_;
};
struct ExtractedDbFields {
  std::string user;
  SecretBytes password;
  UtcInstant expiry; // Exact microseconds, never an implicit milliseconds handoff.
};
class ResponseSnapshot;
FieldOutcome<ExtractedDbFields> extract_db_fields(DbCredentialOperation, ResponseSnapshot&&,
    const Cancellation* = nullptr);
class FieldOccurrence final {
 public:
  static FieldOutcome<FieldOccurrence> create(std::string key, FieldAtom&&);
  FieldOccurrence(const FieldOccurrence&) = delete;
  FieldOccurrence& operator=(const FieldOccurrence&) = delete;
  FieldOccurrence(FieldOccurrence&&);
  FieldOccurrence& operator=(FieldOccurrence&&) = delete;
  const std::string& key() const noexcept { return key_; }
  const FieldAtom& atom() const noexcept { return atom_; }
  std::optional<FieldError> invariant_error() const noexcept;
 private:
  friend FieldOutcome<ExtractedDbFields> extract_db_fields(DbCredentialOperation, ResponseSnapshot&&, const Cancellation*);
  FieldOccurrence(std::string key, FieldAtom atom) : key_(std::move(key)), atom_(std::move(atom)) {}
  std::string key_;
  FieldAtom atom_;
  bool owns_{true}; // Local moved-from validity only, never provenance.
};
class ResponseSnapshot final {
 public:
  static constexpr std::size_t max_fields = 8, max_key_bytes = 64, max_lexeme_bytes = 64;
  static constexpr std::size_t max_user_bytes = 1024, max_aggregate_bytes = 70 * 1024;
  static FieldOutcome<ResponseSnapshot> create(ResponseShape, std::vector<FieldOccurrence>&&);
  ResponseSnapshot(const ResponseSnapshot&) = delete;
  ResponseSnapshot& operator=(const ResponseSnapshot&) = delete;
  ResponseSnapshot(ResponseSnapshot&&) noexcept;
  ResponseSnapshot& operator=(ResponseSnapshot&&) = delete;
  ResponseShape shape() const noexcept { return shape_; }
  const std::vector<FieldOccurrence>& occurrences() const noexcept { return occurrences_; }
  std::optional<FieldError> invariant_error() const noexcept;
 private:
  friend FieldOutcome<ExtractedDbFields> extract_db_fields(DbCredentialOperation, ResponseSnapshot&&, const Cancellation*);
  ResponseSnapshot(ResponseShape shape, std::vector<FieldOccurrence> fields)
      : shape_(shape), occurrences_(std::move(fields)) {}
  ResponseShape shape_;
  std::vector<FieldOccurrence> occurrences_;
  bool owns_{true};
};
// Local allocation bounds/syntax only; future ingress owns preallocation/parser
// envelope/duplicate/type retention, and issuer owns authenticated operation pairing.
// All input text is wiping storage. SecretBytes thread-confined active-consumption
// move rules apply. Consume before every refusal; fixed safe errors only.
} // namespace rs::core::auth
