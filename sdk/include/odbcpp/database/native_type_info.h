#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace rs::core::database {

// Normalized scalar families; these are not database IDs or ODBC constants.
enum class ScalarType {
  Boolean, Binary, Char, VarChar, BigInt, SmallInt, Integer, Real, Double,
  Date, Time, Timestamp, Numeric, Decimal, LongVarChar, LongVarBinary
};

// Presentation only: neither setting truncates values or changes wire types.
// A neutral policy preserves native metadata. Providers select product defaults.
struct CharacterMetadataPolicy {
  std::uint32_t varchar_threshold{0};
  std::uint32_t long_varchar_size{0};
  bool boolean_as_character{false};
  friend bool operator==(const CharacterMetadataPolicy&,
                         const CharacterMetadataPolicy&) = default;
};

inline bool valid_character_metadata_policy(const CharacterMetadataPolicy& policy) noexcept {
  constexpr auto maximum = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
  return policy.varchar_threshold <= maximum && policy.long_varchar_size <= maximum;
}

struct NativeTypeInfo {
  ScalarType type{ScalarType::VarChar};
  std::uint64_t column_size{0};
  std::int16_t decimal_digits{0};
  // False means the backend used its fallback, e.g. an unresolved user type.
  bool known{false};
  // When presentation changes family, retain the native family for type names.
  // Parameter metadata remains native and does not acquire this presentation tag.
  std::optional<ScalarType> declared_type{};
  std::optional<std::uint64_t> declared_column_size{};
  // Owning backend type name for TYPE_NAME presentation when the family's
  // catalog name does not identify the native type. Never a native ID or codec.
  std::optional<std::string> type_name{};
};

inline NativeTypeInfo apply_character_metadata_policy(
    NativeTypeInfo native, const CharacterMetadataPolicy& policy) noexcept {
  if (native.known && native.type == ScalarType::Boolean && policy.boolean_as_character) {
    native.declared_type = ScalarType::Boolean;
    native.declared_column_size = native.column_size;
    native.type = ScalarType::VarChar;
    native.column_size = 1;
    return native;
  }
  if (!native.known || policy.varchar_threshold == 0 ||
      native.column_size <= policy.varchar_threshold ||
      (native.type != ScalarType::Char && native.type != ScalarType::VarChar))
    return native;
  native.declared_type = native.type;
  native.declared_column_size = native.column_size;
  native.type = ScalarType::LongVarChar;
  if (policy.long_varchar_size != 0) native.column_size = policy.long_varchar_size;
  return native;
}

// Backend catalog normalization consumes native dimensions, before constructing
// its prescribed result schema. Restore those dimensions without touching cells.
inline NativeTypeInfo declared_character_metadata(NativeTypeInfo native) noexcept {
  if (native.declared_type && native.declared_column_size) {
    native.type = *native.declared_type;
    native.column_size = *native.declared_column_size;
    native.declared_type.reset();
    native.declared_column_size.reset();
  }
  return native;
}

} // namespace rs::core::database
