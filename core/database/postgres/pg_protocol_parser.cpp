#include "pg_protocol_parser.h"
#include <cstring>
#include <openssl/evp.h>

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
    std::span<const std::string> params) {
  // Simplified: just use simple query for now
  return create_simple_query(sql);
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

std::vector<std::vector<std::string>> PgProtocolParser::extract_query_results(
    const std::vector<Message>& messages) {
  
  std::vector<std::vector<std::string>> rows;
  
  for (const auto& msg : messages) {
    if (msg.tag == 'D') { // DataRow
      const auto* p = reinterpret_cast<const unsigned char*>(msg.payload.data());
      size_t n = msg.payload.size();
      
      if (n < 2) continue;
      
      uint16_t ncols = (p[0] << 8) | p[1];
      size_t off = 2;
      
      std::vector<std::string> row;
      row.reserve(ncols);
      
      for (uint16_t i = 0; i < ncols && off + 4 <= n; ++i) {
        int32_t clen = (p[off] << 24) | (p[off+1] << 16) | (p[off+2] << 8) | p[off+3];
        off += 4;
        
        if (clen < 0) {
          row.emplace_back(); // NULL
        } else if (off + clen <= n) {
          row.emplace_back(reinterpret_cast<const char*>(p + off), clen);
          off += clen;
        }
      }
      
      rows.emplace_back(std::move(row));
    }
  }
  
  return rows;
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