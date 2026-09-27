#include "pg_protocol_parser.h"

namespace rs::core::database::postgres {

NativeTypeInfo PgProtocolParser::describe_type(
    std::uint32_t oid, std::int16_t type_size,
    std::int32_t type_modifier) const {
  switch (oid) {
    case 16: return {ScalarType::Boolean, 1, 0, true};
    case 17: return {ScalarType::Binary, type_size > 0 ? static_cast<std::uint64_t>(type_size) : 0, 0, true};
    case 18: return {ScalarType::Char, 1, 0, true};
    case 19: return {ScalarType::VarChar, 63, 0, true};
    case 20: return {ScalarType::BigInt, 19, 0, true};
    case 21: return {ScalarType::SmallInt, 5, 0, true};
    case 23: return {ScalarType::Integer, 10, 0, true};
    case 25: return {ScalarType::VarChar, 0, 0, true};
    case 26: return {ScalarType::BigInt, 10, 0, true};
    case 700: return {ScalarType::Real, 7, 6, true};
    case 701: return {ScalarType::Double, 15, 15, true};
    case 1042:
    case 1043: {
      const auto length = type_modifier >= 4
          ? static_cast<std::uint64_t>(type_modifier - 4) : 0;
      return {oid == 1042 ? ScalarType::Char : ScalarType::VarChar, length, 0, true};
    }
    case 1082: return {ScalarType::Date, 10, 0, true};
    case 1083:
    case 1266: {
      const auto precision = type_modifier >= 0 ? type_modifier : 6;
      return {ScalarType::Time,
              static_cast<std::uint64_t>(oid == 1083 ? 8 : 14) +
                  (precision > 0 ? 1 + static_cast<std::uint64_t>(precision) : 0),
              static_cast<std::int16_t>(precision), true};
    }
    case 1114:
    case 1184: {
      const auto precision = type_modifier >= 0 ? type_modifier : 6;
      return {ScalarType::Timestamp,
              static_cast<std::uint64_t>(oid == 1114 ? 19 : 25) +
                  (precision > 0 ? 1 + static_cast<std::uint64_t>(precision) : 0),
              static_cast<std::int16_t>(precision), true};
    }
    case 2950: return {ScalarType::VarChar, 36, 0, true};
    case 114:
    case 3802: return {ScalarType::VarChar, 0, 0, true};
    case 1700: {
      if (type_modifier < 4) return {ScalarType::Numeric, 0, 0, true};
      const auto modifier = static_cast<std::uint32_t>(type_modifier - 4);
      const auto precision = static_cast<std::uint64_t>((modifier >> 16) & 0xffff);
      const auto encoded_scale = static_cast<std::int32_t>(modifier & 0x7ff);
      const auto scale = static_cast<std::int16_t>(
          encoded_scale >= 1024 ? encoded_scale - 2048 : encoded_scale);
      return {ScalarType::Numeric, precision, scale, true};
    }
    default:
      return {ScalarType::VarChar,
              type_size > 0 ? static_cast<std::uint64_t>(type_size) : 0, 0, false};
  }
}

} // namespace rs::core::database::postgres
