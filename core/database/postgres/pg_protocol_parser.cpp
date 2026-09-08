#include "pg_protocol_parser.h"
#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>
#include <openssl/evp.h>

namespace {

using rs::core::database::QueryParameterType;

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
  out.push_back(static_cast<std::byte>(value & 0xff));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
  out.push_back(static_cast<std::byte>((value >> 24) & 0xff));
  out.push_back(static_cast<std::byte>((value >> 16) & 0xff));
  out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
  out.push_back(static_cast<std::byte>(value & 0xff));
}

void append_cstring(std::vector<std::byte>& out, std::string_view value) {
  if (!value.empty()) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    out.insert(out.end(), first, first + value.size());
  }
  out.push_back(std::byte{0});
}

std::size_t begin_message(std::vector<std::byte>& out, char tag) {
  const auto start = out.size();
  out.push_back(static_cast<std::byte>(tag));
  append_u32(out, 0);
  return start;
}

void finish_message(std::vector<std::byte>& out, std::size_t start) {
  const auto length = out.size() - start - 1;
  if (length > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("PostgreSQL message is too large");
  }
  const auto value = static_cast<std::uint32_t>(length);
  out[start + 1] = static_cast<std::byte>((value >> 24) & 0xff);
  out[start + 2] = static_cast<std::byte>((value >> 16) & 0xff);
  out[start + 3] = static_cast<std::byte>((value >> 8) & 0xff);
  out[start + 4] = static_cast<std::byte>(value & 0xff);
}

std::uint32_t postgres_type_oid(QueryParameterType type) {
  switch (type) {
    case QueryParameterType::Unspecified: return 0;
    case QueryParameterType::Boolean: return 16;
    case QueryParameterType::Binary: return 17;
    case QueryParameterType::Int64: return 20;
    case QueryParameterType::Int32: return 23;
    case QueryParameterType::Text: return 25;
    case QueryParameterType::Float64: return 701;
    case QueryParameterType::Numeric: return 1700;
  }
  return 0;
}

std::uint16_t read_u16(std::span<const std::byte> data, std::size_t offset) {
  if (offset + 2 > data.size()) {
    throw std::runtime_error("truncated PostgreSQL message");
  }
  return (static_cast<std::uint16_t>(data[offset]) << 8) |
         static_cast<std::uint16_t>(data[offset + 1]);
}

std::uint32_t read_u32(std::span<const std::byte> data, std::size_t offset) {
  if (offset + 4 > data.size()) {
    throw std::runtime_error("truncated PostgreSQL message");
  }
  return (static_cast<std::uint32_t>(data[offset]) << 24) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
         static_cast<std::uint32_t>(data[offset + 3]);
}

std::string read_cstring(std::span<const std::byte> data, std::size_t& offset) {
  const auto start = offset;
  while (offset < data.size() && data[offset] != std::byte{0}) ++offset;
  if (offset == data.size()) {
    throw std::runtime_error("unterminated PostgreSQL metadata string");
  }
  std::string value(reinterpret_cast<const char*>(data.data() + start),
                    offset - start);
  ++offset;
  return value;
}

std::size_t command_affected_rows(std::string_view command_tag) {
  const auto separator = command_tag.find_last_of(' ');
  const auto number = separator == std::string_view::npos
      ? command_tag : command_tag.substr(separator + 1);
  if (number.empty() || !std::isdigit(static_cast<unsigned char>(number[0]))) {
    return 0;
  }
  std::size_t rows = 0;
  const auto [end, error] = std::from_chars(
      number.data(), number.data() + number.size(), rows);
  return error == std::errc{} && end == number.data() + number.size() ? rows : 0;
}

bool is_dollar_tag_start(char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

bool is_dollar_tag_continue(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

std::string replace_parameter_markers(std::string_view sql,
                                      std::size_t parameter_count) {
  enum class State { Normal, SingleQuote, DoubleQuote, LineComment, BlockComment };
  State state = State::Normal;
  std::size_t block_depth = 0;
  std::size_t marker_count = 0;
  std::string dollar_delimiter;
  std::string out;
  out.reserve(sql.size() + parameter_count * 2);

  for (std::size_t i = 0; i < sql.size();) {
    if (!dollar_delimiter.empty()) {
      if (sql.substr(i).starts_with(dollar_delimiter)) {
        out.append(dollar_delimiter);
        i += dollar_delimiter.size();
        dollar_delimiter.clear();
      } else {
        out.push_back(sql[i++]);
      }
      continue;
    }

    const char ch = sql[i];
    const char next = i + 1 < sql.size() ? sql[i + 1] : '\0';

    if (state == State::LineComment) {
      out.push_back(ch);
      ++i;
      if (ch == '\n') state = State::Normal;
      continue;
    }
    if (state == State::BlockComment) {
      if (ch == '/' && next == '*') {
        out.append("/*");
        i += 2;
        ++block_depth;
      } else if (ch == '*' && next == '/') {
        out.append("*/");
        i += 2;
        if (--block_depth == 0) state = State::Normal;
      } else {
        out.push_back(ch);
        ++i;
      }
      continue;
    }
    if (state == State::SingleQuote || state == State::DoubleQuote) {
      const char quote = state == State::SingleQuote ? '\'' : '"';
      out.push_back(ch);
      ++i;
      if (ch == quote) {
        if (i < sql.size() && sql[i] == quote) {
          out.push_back(sql[i++]);
        } else {
          state = State::Normal;
        }
      } else if (ch == '\\' && i < sql.size()) {
        out.push_back(sql[i++]);
      }
      continue;
    }

    if (ch == '\'' || ch == '"') {
      state = ch == '\'' ? State::SingleQuote : State::DoubleQuote;
      out.push_back(ch);
      ++i;
    } else if (ch == '-' && next == '-') {
      state = State::LineComment;
      out.append("--");
      i += 2;
    } else if (ch == '/' && next == '*') {
      state = State::BlockComment;
      block_depth = 1;
      out.append("/*");
      i += 2;
    } else if (ch == '$') {
      std::size_t end = i + 1;
      if (end < sql.size() && is_dollar_tag_start(sql[end])) {
        while (end < sql.size() && is_dollar_tag_continue(sql[end])) ++end;
      }
      if (end < sql.size() && sql[end] == '$') {
        dollar_delimiter.assign(sql.substr(i, end - i + 1));
        out.append(dollar_delimiter);
        i = end + 1;
      } else {
        out.push_back(ch);
        ++i;
      }
    } else if (ch == '?') {
      ++marker_count;
      out.push_back('$');
      out.append(std::to_string(marker_count));
      ++i;
    } else {
      out.push_back(ch);
      ++i;
    }
  }

  if (marker_count != 0 && marker_count != parameter_count) {
    throw std::invalid_argument(
        "parameter marker count does not match bound parameter count");
  }
  return out;
}

} // namespace

namespace rs::core::database::postgres {

std::vector<std::byte> PgProtocolParser::create_startup_message(
    const std::string& user, 
    const std::string& database,
    const std::map<std::string, std::string>& params) {
  
  std::vector<std::pair<std::string, std::string>> kv;
  kv.push_back({"user", user});
  kv.push_back({"database", database});
  for (const auto& p : params) {
    kv.push_back(p);
  }
  
  size_t bytes = 4 + 4 + 1; // len + protocol + terminator
  for (const auto& p : kv) {
    bytes += p.first.size() + 1 + p.second.size() + 1;
  }
  
  std::vector<std::byte> buf(bytes);
  auto* data = reinterpret_cast<unsigned char*>(buf.data());
  
  // Length
  uint32_t len = static_cast<uint32_t>(bytes);
  data[0] = (len >> 24) & 0xFF;
  data[1] = (len >> 16) & 0xFF; 
  data[2] = (len >> 8) & 0xFF;
  data[3] = len & 0xFF;
  
  // Protocol 3.0
  uint32_t proto = 196608;
  data[4] = (proto >> 24) & 0xFF;
  data[5] = (proto >> 16) & 0xFF;
  data[6] = (proto >> 8) & 0xFF;
  data[7] = proto & 0xFF;
  
  size_t off = 8;
  for (const auto& p : kv) {
    std::memcpy(data + off, p.first.data(), p.first.size());
    off += p.first.size();
    data[off++] = 0;
    std::memcpy(data + off, p.second.data(), p.second.size());
    off += p.second.size();
    data[off++] = 0;
  }
  data[off] = 0;
  
  return buf;
}

std::vector<std::byte> PgProtocolParser::create_ssl_request() {
  std::vector<std::byte> buf(8);
  auto* data = reinterpret_cast<unsigned char*>(buf.data());
  
  // Length = 8
  data[0] = 0; data[1] = 0; data[2] = 0; data[3] = 8;
  // SSL request code = 80877103
  uint32_t code = 80877103;
  data[4] = (code >> 24) & 0xFF;
  data[5] = (code >> 16) & 0xFF;
  data[6] = (code >> 8) & 0xFF;
  data[7] = code & 0xFF;
  
  return buf;
}

AuthenticationRequest PgProtocolParser::parse_auth_request(const std::vector<std::byte>& data) {
  if (data.size() < 4) throw std::runtime_error("Auth payload too short");
  
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  uint32_t code = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  
  AuthenticationRequest req;
  switch (code) {
    case 0: req.type = AuthenticationRequest::Type::None; break;
    case 3: req.type = AuthenticationRequest::Type::Cleartext; break;
    case 5: 
      req.type = AuthenticationRequest::Type::MD5;
      if (data.size() >= 8) {
        req.challenge_data.assign(data.begin() + 4, data.begin() + 8);
      }
      break;
    default: throw std::runtime_error("Unsupported auth method: " + std::to_string(code));
  }
  
  return req;
}

std::vector<std::byte> PgProtocolParser::create_auth_response(
    const AuthenticationRequest& request,
    const std::string& password,
    const std::string& user) {
  
  std::string auth_string;
  
  switch (request.type) {
    case AuthenticationRequest::Type::None:
      return {};
      
    case AuthenticationRequest::Type::Cleartext:
      auth_string = password;
      break;
      
    case AuthenticationRequest::Type::MD5: {
      std::string step1 = md5_hex((password + user).data(), password.size() + user.size());
      std::string salted = step1 + std::string(reinterpret_cast<const char*>(request.challenge_data.data()), 4);
      std::string step2 = md5_hex(salted.data(), salted.size());
      auth_string = "md5" + step2;
      break;
    }
      
    default:
      throw std::runtime_error("Unsupported auth type");
  }
  
  // Create password message: 'p' + len + password + '\0'
  size_t total_len = 1 + 4 + auth_string.size() + 1;
  std::vector<std::byte> buf(total_len);
  auto* data = reinterpret_cast<unsigned char*>(buf.data());
  
  data[0] = 'p';
  uint32_t len = static_cast<uint32_t>(4 + auth_string.size() + 1);
  data[1] = (len >> 24) & 0xFF;
  data[2] = (len >> 16) & 0xFF;
  data[3] = (len >> 8) & 0xFF;
  data[4] = len & 0xFF;
  
  std::memcpy(data + 5, auth_string.data(), auth_string.size());
  data[5 + auth_string.size()] = 0;
  
  return buf;
}

std::vector<std::byte> PgProtocolParser::create_simple_query(std::string_view sql) {
  size_t total_len = 1 + 4 + sql.size() + 1;
  std::vector<std::byte> buf(total_len);
  auto* data = reinterpret_cast<unsigned char*>(buf.data());
  
  data[0] = 'Q';
  uint32_t len = static_cast<uint32_t>(4 + sql.size() + 1);
  data[1] = (len >> 24) & 0xFF;
  data[2] = (len >> 16) & 0xFF;
  data[3] = (len >> 8) & 0xFF;
  data[4] = len & 0xFF;
  
  std::memcpy(data + 5, sql.data(), sql.size());
  data[5 + sql.size()] = 0;
  
  return buf;
}

std::vector<std::byte> PgProtocolParser::create_prepared_query(
    std::string_view sql,
    std::span<const QueryParameter> params) {
  if (params.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::length_error("too many PostgreSQL query parameters");
  }

  const std::string rewritten_sql = replace_parameter_markers(sql, params.size());
  std::vector<std::byte> out;
  out.reserve(rewritten_sql.size() + 64);

  // Parse the unnamed statement and supply protocol-native type hints.
  auto start = begin_message(out, 'P');
  append_cstring(out, {});
  append_cstring(out, rewritten_sql);
  append_u16(out, static_cast<std::uint16_t>(params.size()));
  for (const auto& param : params) append_u32(out, postgres_type_oid(param.type));
  finish_message(out, start);

  // Describing the statement produces ParameterDescription before binding.
  start = begin_message(out, 'D');
  out.push_back(std::byte{'S'});
  append_cstring(out, {});
  finish_message(out, start);

  // Bind text-format values to the unnamed portal. A length of -1 is SQL NULL.
  start = begin_message(out, 'B');
  append_cstring(out, {});
  append_cstring(out, {});
  append_u16(out, 0); // all parameter values use text format
  append_u16(out, static_cast<std::uint16_t>(params.size()));
  for (const auto& param : params) {
    if (!param.value.has_value()) {
      append_u32(out, std::numeric_limits<std::uint32_t>::max());
      continue;
    }
    if (param.value->size() >
        static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
      throw std::length_error("PostgreSQL parameter value is too large");
    }
    append_u32(out, static_cast<std::uint32_t>(param.value->size()));
    if (!param.value->empty()) {
      const auto* first = reinterpret_cast<const std::byte*>(param.value->data());
      out.insert(out.end(), first, first + param.value->size());
    }
  }
  append_u16(out, 0); // all result columns use text format
  finish_message(out, start);

  start = begin_message(out, 'D');
  out.push_back(std::byte{'P'}); // describe the unnamed portal
  append_cstring(out, {});
  finish_message(out, start);

  start = begin_message(out, 'E');
  append_cstring(out, {});
  append_u32(out, 0); // no row limit
  finish_message(out, start);

  start = begin_message(out, 'S');
  finish_message(out, start);
  return out;
}

Message PgProtocolParser::parse_message(const std::vector<std::byte>& data) {
  if (data.size() < 5) throw std::runtime_error("Message too short");
  
  Message msg;
  msg.tag = static_cast<char>(data[0]);
  
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  uint32_t len = (p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4];
  
  if (len < 4 || data.size() < len + 1) {
    throw std::runtime_error("Invalid message length");
  }
  
  msg.payload.assign(data.begin() + 5, data.begin() + 1 + len);
  return msg;
}

bool PgProtocolParser::is_ready_for_query(const Message& msg) {
  return msg.tag == 'Z';
}

bool PgProtocolParser::is_error_response(const Message& msg) {
  return msg.tag == 'E';
}

std::string PgProtocolParser::extract_error_message(const Message& msg) {
  if (msg.tag != 'E') return "";
  
  auto error = decode_error(msg.payload);
  return error.message();
}

ResultRows PgProtocolParser::extract_query_results(
    const std::vector<Message>& messages) {
  ResultRows rows;

  for (const auto& msg : messages) {
    if (msg.tag == 'D') { // DataRow
      const std::span<const std::byte> payload(msg.payload);
      std::size_t offset = 0;
      const auto column_count = read_u16(payload, offset);
      offset += 2;

      ResultRow row;
      row.reserve(column_count);

      for (std::uint16_t i = 0; i < column_count; ++i) {
        const auto length = read_u32(payload, offset);
        offset += 4;

        if (length == 0xffffffffu) {
          row.emplace_back(std::nullopt);
          continue;
        }
        if (length > 0x7fffffffu || length > payload.size() - offset) {
          throw std::runtime_error("invalid PostgreSQL DataRow column length");
        }
        row.emplace_back(std::string(
            reinterpret_cast<const char*>(payload.data() + offset), length));
        offset += length;
      }

      if (offset != payload.size()) {
        throw std::runtime_error("invalid PostgreSQL DataRow length");
      }
      rows.emplace_back(std::move(row));
    }
  }

  return rows;
}

QueryResult PgProtocolParser::extract_query_result(
    const std::vector<Message>& messages) {
  QueryResult result;
  result.rows = extract_query_results(messages);

  for (const auto& message : messages) {
    const std::span<const std::byte> payload(message.payload);
    if (message.tag == 'T') { // RowDescription
      std::size_t offset = 0;
      const auto count = read_u16(payload, offset);
      offset += 2;
      std::vector<ResultColumnMetadata> columns;
      columns.reserve(count);
      for (std::uint16_t i = 0; i < count; ++i) {
        ResultColumnMetadata column;
        column.name = read_cstring(payload, offset);
        column.table_id = read_u32(payload, offset);
        offset += 4;
        column.table_column = static_cast<std::int16_t>(read_u16(payload, offset));
        offset += 2;
        column.type_id = read_u32(payload, offset);
        offset += 4;
        column.type_size = static_cast<std::int16_t>(read_u16(payload, offset));
        offset += 2;
        column.type_modifier = static_cast<std::int32_t>(read_u32(payload, offset));
        offset += 4;
        column.format_code = static_cast<std::int16_t>(read_u16(payload, offset));
        offset += 2;
        columns.push_back(std::move(column));
      }
      if (offset != payload.size()) {
        throw std::runtime_error("invalid PostgreSQL RowDescription length");
      }
      result.columns = std::move(columns);
    } else if (message.tag == 't') { // ParameterDescription
      std::size_t offset = 0;
      const auto count = read_u16(payload, offset);
      offset += 2;
      std::vector<std::uint32_t> parameter_types;
      parameter_types.reserve(count);
      for (std::uint16_t i = 0; i < count; ++i) {
        parameter_types.push_back(read_u32(payload, offset));
        offset += 4;
      }
      if (offset != payload.size()) {
        throw std::runtime_error("invalid PostgreSQL ParameterDescription length");
      }
      result.parameter_type_ids = std::move(parameter_types);
    } else if (message.tag == 'C') { // CommandComplete
      std::size_t offset = 0;
      result.command_tag = read_cstring(payload, offset);
      if (offset != payload.size()) {
        throw std::runtime_error("invalid PostgreSQL CommandComplete length");
      }
      result.affected_rows = command_affected_rows(result.command_tag);
    }
  }
  return result;
}

std::string PgProtocolParser::md5_hex(const void* data, size_t n) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int mdlen = 0;
  
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
  EVP_DigestUpdate(ctx, data, n);
  EVP_DigestFinal_ex(ctx, md, &mdlen);
  EVP_MD_CTX_free(ctx);
  
  static const char hexd[] = "0123456789abcdef";
  std::string out;
  out.resize(mdlen * 2);
  for (unsigned i = 0; i < mdlen; i++) {
    out[2*i] = hexd[(md[i] >> 4) & 0xF];
    out[2*i+1] = hexd[md[i] & 0xF];
  }
  
  return out;
}

rs::pg::Authentication PgProtocolParser::decode_auth(const std::vector<std::byte>& payload) {
  if (payload.size() < 4) throw std::runtime_error("Auth payload too short");
  
  rs::pg::Authentication a{};
  const auto* p = reinterpret_cast<const unsigned char*>(payload.data());
  a.raw_code = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
  
  if (a.raw_code == 5 && payload.size() >= 8) {
    std::memcpy(a.md5_salt.data(), payload.data() + 4, 4);
  }
  
  return a;
}

rs::pg::ErrorResponse PgProtocolParser::decode_error(const std::vector<std::byte>& payload) {
  rs::pg::ErrorResponse e{};
  const auto* p = reinterpret_cast<const unsigned char*>(payload.data());
  size_t i = 0, n = payload.size();
  
  while (i < n && p[i] != 0) {
    char code = static_cast<char>(p[i++]);
    size_t start = i;
    while (i < n && p[i] != 0) ++i;
    
    std::string val(reinterpret_cast<const char*>(p + start), i - start);
    if (i < n && p[i] == 0) ++i;
    
    e.fields[code] = std::move(val);
  }
  
  return e;
}

} // namespace rs::core::database::postgres
