#pragma once

#include "core/database/mysql/date_wire.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rs::core::database::mysql::date_parameter_detail {
// Private encoder candidate only: prepared parameter admission is unchanged.
// Absence means a typed DATE NULL: set the parameter bitmap, send no value
// bytes. An engaged value owns the raw length byte and four calendar bytes.
struct Parameter {
  static constexpr std::uint8_t native_type=10;
  static constexpr std::uint8_t unsigned_flag=0;
  std::optional<std::array<std::byte,5>> value;
};

inline rs::util::Result<Parameter> encode(std::optional<std::string_view> input) {
  if (!input) return Parameter{std::nullopt};
  // Validate the entire caller value before producing any encoded bytes.
  // Invalid input is never a deferred result-cell error or coerced to NULL.
  if (!date_detail::valid_cell(*input)) return {rs::util::DbErrorCode::InvalidParameter};
  unsigned year{},month{},day{};
  for (std::size_t i=0;i<input->size();++i) {
    if (i==4 || i==7) continue;
    auto& part=i<4?year:(i<7?month:day);
    part=part*10+static_cast<unsigned>((*input)[i]-'0');
  }
  return Parameter{std::array<std::byte,5>{std::byte{4},
      static_cast<std::byte>(year&255),static_cast<std::byte>(year>>8),
      static_cast<std::byte>(month),static_cast<std::byte>(day)}};
}
}
