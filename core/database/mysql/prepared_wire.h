#pragma once

#include "core/database/mysql/query_wire.h"
#include "core/database/mysql/handshake_wire.h"
#include "core/database/mysql/date_parameter_wire.h"

#include <array>
#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>

namespace rs::core::database::mysql::prepared_detail {

// Protocol shapes:
// https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_stmt_prepare.html
// https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_com_stmt_execute.html
// https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_binary_resultset_row.html
struct PrepareInfo {
  std::uint32_t statement_id{};
  std::uint16_t columns{};
  std::uint16_t parameters{};
};

using NativeColumn=query_detail::NativeColumn;

// Private packet candidate only. Live callers retain the existing profile.
enum class ParameterProfile { ExistingOnly, DateCandidate };

namespace detail {

using rs::util::DbErrorCode;

inline std::uint64_t little(std::span<const std::byte> bytes) {
  std::uint64_t value{};
  for (std::size_t i = 0; i < bytes.size(); ++i)
    value |= std::uint64_t(std::to_integer<unsigned>(bytes[i])) << (8 * i);
  return value;
}

inline void append_little(std::vector<std::byte>& output, std::uint64_t value,
                          std::size_t width) {
  for (std::size_t i = 0; i < width; ++i)
    output.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
}

inline std::size_t length_size(std::size_t size) {
  if (size < 251) return 1;
  if (size <= 0xffff) return 3;
  if (size <= 0xffffff) return 4;
  return 9;
}

inline void append_length(std::vector<std::byte>& output, std::size_t size) {
  if (size < 251) {
    output.push_back(static_cast<std::byte>(size));
  } else if (size <= 0xffff) {
    output.push_back(std::byte{252});
    append_little(output, size, 2);
  } else if (size <= 0xffffff) {
    output.push_back(std::byte{253});
    append_little(output, size, 3);
  } else {
    output.push_back(std::byte{254});
    append_little(output, size, 8);
  }
}

inline bool add_size(std::size_t& total, std::size_t amount) {
  if (amount > std::numeric_limits<std::size_t>::max() - total) return false;
  total += amount;
  return true;
}

template <class Integer>
inline bool parse_integer(std::string_view input, Integer& value) {
  if (input.empty()) return false;
  const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size();
}

inline rs::util::Result<std::pair<std::uint8_t, std::size_t>> parameter_shape(
    const QueryParameter& parameter,ParameterProfile profile=ParameterProfile::ExistingOnly) {
  if (parameter.binary_input && parameter.type != QueryParameterType::Binary)
    return {DbErrorCode::InvalidParameter};
  switch (parameter.type) {
    case QueryParameterType::Int16: return std::pair<std::uint8_t, std::size_t>{2, 2};
    case QueryParameterType::Int32: return std::pair<std::uint8_t, std::size_t>{3, 4};
    case QueryParameterType::Int64: return std::pair<std::uint8_t, std::size_t>{8, 8};
    case QueryParameterType::Boolean: return std::pair<std::uint8_t, std::size_t>{1, 1};
    case QueryParameterType::Text:
    case QueryParameterType::Unspecified:
      if (parameter.value && !rs::util::utf8_code_point_count(*parameter.value))
        return {DbErrorCode::InvalidParameter};
      return std::pair<std::uint8_t, std::size_t>{253,
          parameter.value ? length_size(parameter.value->size()) + parameter.value->size() : 0};
    case QueryParameterType::Binary:
      return std::pair<std::uint8_t, std::size_t>{252,
          parameter.value ? length_size(parameter.value->size()) + parameter.value->size() : 0};
    case QueryParameterType::Date: {
      if (profile!=ParameterProfile::DateCandidate) return {DbErrorCode::UnsupportedFeature};
      const auto date=date_parameter_detail::encode(parameter.value
          ?std::optional<std::string_view>(*parameter.value):std::nullopt);
      if (!date) return {date.error()};
      return std::pair<std::uint8_t,std::size_t>{date->native_type,date->value?date->value->size():0};
    }
    case QueryParameterType::Float32:
    case QueryParameterType::Float64:
    case QueryParameterType::Numeric:
    case QueryParameterType::Time:
    case QueryParameterType::Timestamp:
      return {DbErrorCode::UnsupportedFeature};
  }
  return {DbErrorCode::UnsupportedFeature};
}

inline bool validate_scalar(const QueryParameter& parameter) {
  if (!parameter.value) return true;
  switch (parameter.type) {
    case QueryParameterType::Int16: {
      std::int16_t value{}; return parse_integer(*parameter.value, value);
    }
    case QueryParameterType::Int32: {
      std::int32_t value{}; return parse_integer(*parameter.value, value);
    }
    case QueryParameterType::Int64: {
      std::int64_t value{}; return parse_integer(*parameter.value, value);
    }
    case QueryParameterType::Boolean:
      return *parameter.value == "0" || *parameter.value == "1";
    default: return true;
  }
}

inline void append_parameter_value(std::vector<std::byte>& output,
    const QueryParameter& parameter,ParameterProfile profile=ParameterProfile::ExistingOnly) {
  if (!parameter.value) return;
  switch (parameter.type) {
    case QueryParameterType::Int16: {
      std::int16_t value{}; (void)parse_integer(*parameter.value, value);
      append_little(output, static_cast<std::uint16_t>(value), 2); return;
    }
    case QueryParameterType::Int32: {
      std::int32_t value{}; (void)parse_integer(*parameter.value, value);
      append_little(output, static_cast<std::uint32_t>(value), 4); return;
    }
    case QueryParameterType::Int64: {
      std::int64_t value{}; (void)parse_integer(*parameter.value, value);
      append_little(output, static_cast<std::uint64_t>(value), 8); return;
    }
    case QueryParameterType::Boolean:
      output.push_back(*parameter.value == "1" ? std::byte{1} : std::byte{0}); return;
    case QueryParameterType::Date: {
      if (profile!=ParameterProfile::DateCandidate) return;
      const auto date=date_parameter_detail::encode(std::string_view(*parameter.value));
      if (date && date->value) output.insert(output.end(),date->value->begin(),date->value->end());
      return;
    }
    case QueryParameterType::Text:
    case QueryParameterType::Unspecified:
    case QueryParameterType::Binary:
      append_length(output, parameter.value->size());
      for (const auto byte : *parameter.value)
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
      return;
    default: return;
  }
}

inline std::string decimal(std::uint64_t bits, std::size_t width, bool unsigned_value) {
  std::array<char, 32> output{};
  std::to_chars_result converted{};
  if (unsigned_value) {
    converted = std::to_chars(output.data(), output.data() + output.size(), bits);
  } else {
    std::int64_t value{};
    switch (width) {
      case 1: value = std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(bits)); break;
      case 2: value = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(bits)); break;
      case 4: value = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bits)); break;
      default: value = std::bit_cast<std::int64_t>(bits); break;
    }
    converted = std::to_chars(output.data(), output.data() + output.size(), value);
  }
  return {output.data(), converted.ptr};
}

class BinaryCursor {
 public:
  explicit BinaryCursor(std::span<const std::byte> input) : input_(input) {}
  bool bytes(std::size_t count, std::span<const std::byte>& value) {
    if (count > input_.size() - offset_) return false;
    value = input_.subspan(offset_, count); offset_ += count; return true;
  }
  bool length_bytes(std::span<const std::byte>& value) {
    std::span<const std::byte> first;
    if (!bytes(1, first)) return false;
    const auto marker = std::to_integer<unsigned>(first[0]);
    std::size_t width{};
    if (marker < 251) return bytes(marker, value);
    if (marker == 252) width = 2;
    else if (marker == 253) width = 3;
    else if (marker == 254) width = 8;
    else return false;
    std::span<const std::byte> encoded;
    if (!bytes(width, encoded)) return false;
    const auto size = little(encoded);
    if (size > std::numeric_limits<std::size_t>::max()) return false;
    return bytes(static_cast<std::size_t>(size), value);
  }
  std::size_t remaining() const { return input_.size() - offset_; }
 private:
  std::span<const std::byte> input_;
  std::size_t offset_{};
};

}  // namespace detail

inline rs::util::Result<PrepareInfo> parse_prepare(std::span<const std::byte> bytes) {
  using rs::util::DbErrorCode;
  if (bytes.size() != 12 || bytes[0] != std::byte{0} || bytes[9] != std::byte{0})
    return {DbErrorCode::ProtocolError};
  const auto warnings = detail::little(bytes.subspan(10, 2));
  if (warnings) return {DbErrorCode::UnsupportedFeature};
  return PrepareInfo{static_cast<std::uint32_t>(detail::little(bytes.subspan(1, 4))),
      static_cast<std::uint16_t>(detail::little(bytes.subspan(5, 2))),
      static_cast<std::uint16_t>(detail::little(bytes.subspan(7, 2)))};
}

inline rs::util::Result<std::vector<std::byte>> execute_request(
    std::uint32_t statement_id, std::span<const QueryParameter> parameters,
    const InputLimits& limits,ParameterProfile profile=ParameterProfile::ExistingOnly) {
  using rs::util::DbErrorCode;
  if (parameters.size() > limits.max_parameters || parameters.size() > 65535)
    return {DbErrorCode::ResourceLimit};
  const auto null_bytes = (parameters.size() + 7) / 8;
  std::size_t raw_total{};
  std::size_t value_wire{};
  for (const auto& parameter : parameters) {
    const auto raw = parameter.value ? parameter.value->size() : 0;
    if (raw > limits.max_parameter_bytes || !detail::add_size(raw_total, raw) ||
        raw_total > limits.max_parameter_total_bytes)
      return {DbErrorCode::ResourceLimit};
    auto shape = detail::parameter_shape(parameter,profile);
    if (!shape) return {shape.error()};
    if (!detail::validate_scalar(parameter)) return {DbErrorCode::InvalidParameter};
    if (!detail::add_size(value_wire, parameter.value ? shape->second : 0))
      return {DbErrorCode::ResourceLimit};
  }
  std::size_t payload = 10;
  if (!parameters.empty() &&
      (!detail::add_size(payload, null_bytes) || !detail::add_size(payload, 1) ||
       !detail::add_size(payload, parameters.size() * 2) || !detail::add_size(payload, value_wire)))
    return {DbErrorCode::ResourceLimit};
  std::size_t framed = payload;
  if (!detail::add_size(framed, 4) || payload > connection_packet_limit ||
      framed > limits.max_request_wire_bytes)
    return {DbErrorCode::ResourceLimit};

  std::vector<std::byte> output;
  output.reserve(framed);
  detail::append_little(output, payload, 3);
  output.push_back(std::byte{0});
  output.push_back(std::byte{23});
  detail::append_little(output, statement_id, 4);
  output.push_back(std::byte{0});
  detail::append_little(output, 1, 4);
  if (parameters.empty()) return output;
  const auto bitmap_start = output.size();
  output.resize(output.size() + null_bytes, std::byte{0});
  for (std::size_t i = 0; i < parameters.size(); ++i)
    if (!parameters[i].value)
      output[bitmap_start + i / 8] |= static_cast<std::byte>(1u << (i % 8));
  output.push_back(std::byte{1});
  for (const auto& parameter : parameters) {
    const auto shape = detail::parameter_shape(parameter,profile);
    output.push_back(static_cast<std::byte>(shape->first));
    output.push_back(std::byte{0});
  }
  for (const auto& parameter : parameters) detail::append_parameter_value(output, parameter,profile);
  return output;
}

inline rs::util::Result<ResultRow> binary_row(
    std::span<const std::byte> bytes, std::span<const NativeColumn> native,
    std::span<const ResultColumnMetadata> columns, std::size_t row_index,
    std::vector<CellEncodingError>& errors) {
  using rs::util::DbErrorCode;
  if (native.size() != columns.size() || bytes.empty() || bytes[0] != std::byte{0})
    return {DbErrorCode::ProtocolError};
  const auto bitmap_size = (columns.size() + 9) / 8;
  if (bytes.size() < 1 + bitmap_size) return {DbErrorCode::ProtocolError};
  const auto bitmap = bytes.subspan(1, bitmap_size);
  if (!bitmap.empty() && (std::to_integer<unsigned>(bitmap[0]) & 3))
    return {DbErrorCode::ProtocolError};
  if (!bitmap.empty()) {
    const auto used = (columns.size() + 2) % 8;
    if (used && (std::to_integer<unsigned>(bitmap.back()) & (~((1u << used) - 1u) & 255u)))
      return {DbErrorCode::ProtocolError};
  }
  detail::BinaryCursor cursor(bytes.subspan(1 + bitmap_size));
  ResultRow row;
  row.reserve(columns.size());
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (!columns[i].normalized_type) return {DbErrorCode::ProtocolError};
    switch (native[i].type) {
      case 1: case 2: case 3: case 6: case 8: case 9: case 10: case 12:
      case 15: case 252: case 253: case 254: case 246: break;
      default: return {DbErrorCode::UnsupportedFeature};
    }
    if (native[i].type==10 || columns[i].normalized_type->type==ScalarType::Date) {
      const auto& info=*columns[i].normalized_type;
      if (native[i].type!=10 || !info.known || info.type!=ScalarType::Date || info.column_size!=10 || info.decimal_digits!=0)
        return {DbErrorCode::ProtocolError};
    }
    datetime_detail::Profile datetime_metadata;
    if (native[i].type==12 || columns[i].normalized_type->type==ScalarType::Timestamp) {
      auto profile=query_detail::datetime_profile(*columns[i].normalized_type,native[i]);
      if (!profile) return {profile.error()};
      datetime_metadata=*profile;
    }
    const bool null = (std::to_integer<unsigned>(bitmap[(i + 2) / 8]) &
                       (1u << ((i + 2) % 8))) != 0;
    if (null) { row.emplace_back(std::nullopt); continue; }
    std::size_t width{};
    switch (native[i].type) {
      case 1: width = 1; break;
      case 2: width = 2; break;
      case 3: case 9: width = 4; break;
      case 8: width = 8; break;
      case 6: return {DbErrorCode::ProtocolError};
      case 10: case 12: {
        std::span<const std::byte> length_byte,payload;
        if (!cursor.bytes(1,length_byte)) return {DbErrorCode::ProtocolError};
        const auto length=std::to_integer<unsigned>(length_byte[0]);
        if (length!=0 && length!=4 && length!=7 && length!=11) return {DbErrorCode::ProtocolError};
        if (!cursor.bytes(length,payload)) return {DbErrorCode::ProtocolError};
        std::array<std::byte,12> framed{};framed[0]=length_byte[0];
        std::copy(payload.begin(),payload.end(),framed.begin()+1);
        const auto value=std::span<const std::byte>(framed).first(length+1);
        auto cell=native[i].type==12?datetime_detail::binary_cell(value,datetime_metadata):date_detail::binary_cell(value);
        if (!cell) return {cell.error()};
        if (cell->encoding_error) errors.push_back({row_index,i});
        row.emplace_back(std::move(cell->value));continue;
      }
      case 15: case 253: case 254: case 252: case 246: {
        std::span<const std::byte> value;
        if (!cursor.length_bytes(value)) return {DbErrorCode::ProtocolError};
        std::string owned(reinterpret_cast<const char*>(value.data()), value.size());
        const auto& info=*columns[i].normalized_type;
        if (!(native[i].type==246?decimal_detail::valid_cell(owned,info,native[i].unsigned_value):
              query_detail::valid_cell(owned,info.type))) {
          owned.clear(); errors.push_back({row_index, i});
        }
        row.emplace_back(std::move(owned));
        continue;
      }
      default: return {DbErrorCode::UnsupportedFeature};
    }
    std::span<const std::byte> value;
    if (!cursor.bytes(width, value)) return {DbErrorCode::ProtocolError};
    auto owned=detail::decimal(detail::little(value),width,native[i].unsigned_value);
    if (!query_detail::valid_cell(owned,columns[i].normalized_type->type)) {
      owned.clear();errors.push_back({row_index,i});
    }
    row.emplace_back(std::move(owned));
  }
  if (cursor.remaining()) return {DbErrorCode::ProtocolError};
  return row;
}

}  // namespace rs::core::database::mysql::prepared_detail
