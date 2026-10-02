#pragma once
#include "core/database/query_result.h"
#include "core/database/i_database_connection.h"
#include "core/util/utf8.h"
#include <limits>
#include <charconv>

namespace rs::core::database::mysql {
// Private protocol-41 codec. The proof negotiates neither deprecated EOF,
// session tracking, optional metadata nor multiple results.
namespace query_detail {
using rs::util::DbErrorCode;
class Cursor {
 public:
  explicit Cursor(std::span<const std::byte> bytes) : bytes_(bytes) {}
  std::size_t remaining() const { return bytes_.size()-offset_; }
  bool integer(std::size_t width, std::uint64_t& value) {
    if (width>8 || width>remaining()) return false;
    value=0;
    for (std::size_t i=0;i<width;++i) value|=std::uint64_t(std::to_integer<unsigned>(bytes_[offset_++]))<<(8*i);
    return true;
  }
  bool length(std::uint64_t& value) {
    std::uint64_t first{};
    if (!integer(1,first)) return false;
    if (first<251) { value=first;return true; }
    if (first==252) return integer(2,value);
    if (first==253) return integer(3,value);
    if (first==254) return integer(8,value);
    return false; // NULL has meaning only in row cells; 255 is invalid.
  }
  bool text(std::string_view& value) {
    std::uint64_t size{};
    if (!length(size) || size>remaining()) return false;
    value={reinterpret_cast<const char*>(bytes_.data()+offset_),static_cast<std::size_t>(size)};
    offset_+=static_cast<std::size_t>(size);return true;
  }
  bool null_cell() {
    if (remaining() && bytes_[offset_]==std::byte{251}) { ++offset_;return true; }
    return false;
  }
 private:
  std::span<const std::byte> bytes_;std::size_t offset_{};
};
struct Completion { std::size_t affected{};std::uint16_t status{}; };
inline rs::util::Result<Completion> completion(std::span<const std::byte> bytes,bool eof) {
  Cursor c(bytes);std::uint64_t tag{},affected{},insert{},status{},warnings{};
  if (!c.integer(1,tag)) return {DbErrorCode::ProtocolError};
  if (eof) {
    if (tag!=254 || bytes.size()!=5 || !c.integer(2,warnings) || !c.integer(2,status))
      return {DbErrorCode::ProtocolError};
  } else {
    if (tag!=0 || !c.length(affected) || !c.length(insert) || !c.integer(2,status) || !c.integer(2,warnings))
      return {DbErrorCode::ProtocolError};
  }
  if (affected>std::numeric_limits<std::size_t>::max()) return {DbErrorCode::ResourceLimit};
  // Warning delivery and multiple-result draining require their own contracts.
  if (warnings || (status&(8|0x40|0x80|0x1000|0x4000))) return {DbErrorCode::UnsupportedFeature};
  return Completion{static_cast<std::size_t>(affected),static_cast<std::uint16_t>(status)};
}
inline rs::util::Result<ResultColumnMetadata> column(std::span<const std::byte> bytes,const ResultLimits& limits,std::size_t* metadata_bytes=nullptr) {
  Cursor c(bytes);std::string_view fields[6];std::size_t names{};
  for (auto& field:fields) {
    if (!c.text(field)) return {DbErrorCode::ProtocolError};
    if (field.size()>limits.max_metadata_name_bytes-names) return {DbErrorCode::ResourceLimit};
    names+=field.size();
  }
  std::uint64_t fixed{},charset{},size{},type{},flags{},decimals{},filler{};
  if (fields[0]!="def" || !c.length(fixed) || fixed!=12 || !c.integer(2,charset) || !c.integer(4,size) ||
      !c.integer(1,type) || !c.integer(2,flags) || !c.integer(1,decimals) || !c.integer(2,filler) ||
      filler || c.remaining()) return {DbErrorCode::ProtocolError};
  if (fields[4].size()>limits.max_column_name_bytes) return {DbErrorCode::ResourceLimit};
  if (fields[4].find('\0')!=std::string_view::npos || !rs::util::utf8_code_point_count(fields[4]))
    return {DbErrorCode::ProtocolError};
  NativeTypeInfo info;info.known=true;
  const bool unsigned_value=(flags&32)!=0;
  switch(type) {
    case 1: info={ScalarType::SmallInt,3,0,true};break;
    case 2: info={unsigned_value?ScalarType::Integer:ScalarType::SmallInt,5,0,true};break;
    case 3: info={unsigned_value?ScalarType::BigInt:ScalarType::Integer,10,0,true};break;
    case 8: info={unsigned_value?ScalarType::Numeric:ScalarType::BigInt,unsigned_value?20u:19u,0,true};break;
    case 9: info={ScalarType::Integer,8,0,true};break;
    case 6: info={ScalarType::VarChar,0,0,true};break; // NULL expression.
    case 15: case 253: case 254:
      if (charset==63) info={ScalarType::Binary,size,0,true};
      else if (charset==45 || charset==46 || charset==255)
        info={type==254?ScalarType::Char:ScalarType::VarChar,size/4,0,true};
      else return {DbErrorCode::UnsupportedFeature};
      break;
    default:return {DbErrorCode::UnsupportedFeature};
  }
  if (metadata_bytes) *metadata_bytes=names;
  return ResultColumnMetadata{std::string(fields[4]),info};
}
inline bool valid_cell(std::string_view value,ScalarType type) {
  if (type==ScalarType::Binary) return true;
  if (!rs::util::utf8_code_point_count(value)) return false;
  if (type==ScalarType::BigInt || type==ScalarType::Integer || type==ScalarType::SmallInt) {
    std::int64_t number{};const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
    if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) return false;
    if (type==ScalarType::Integer && (number<-2147483648LL || number>2147483647LL)) return false;
    if (type==ScalarType::SmallInt && (number<-32768 || number>32767)) return false;
  } else if (type==ScalarType::Numeric) {
    std::uint64_t number{};const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
    if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) return false;
  }
  return true;
}
inline rs::util::Result<ResultRow> row(std::span<const std::byte> bytes,const std::vector<ResultColumnMetadata>& columns,
    std::size_t index,std::vector<CellEncodingError>& errors) {
  Cursor c(bytes);ResultRow result;result.reserve(columns.size());
  for (std::size_t i=0;i<columns.size();++i) {
    if (c.null_cell()) { result.emplace_back(std::nullopt);continue; }
    std::string_view value;
    if (!c.text(value)) return {DbErrorCode::ProtocolError};
    if (!valid_cell(value,columns[i].normalized_type->type)) {
      result.emplace_back(std::string{});errors.push_back({index,i});
    } else result.emplace_back(std::string(value));
  }
  if (c.remaining()) return {DbErrorCode::ProtocolError};
  return result;
}
}
}
