#pragma once

// Connection-string field parsing for fixtures that read their configured
// connection string. Direct builds use the driver's parser unchanged. Driver
// Manager builds cannot link it, so they use this copy of its rules: fields
// split on ';', trimmed keys upper-cased, values trimmed or braced with '}}'
// as an escaped '}', the first occurrence of a key kept, and an embedded NUL
// or unterminated brace rejected with std::invalid_argument.
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <string>

namespace odbcpp::test::connection_string {

inline std::string trim(const std::string& text) {
  const auto start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(start, end - start + 1);
}

inline std::string to_upper(const std::string& text) {
  std::string result = text;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
  return result;
}

inline std::map<std::string, std::string> parse(const std::string& text) {
  if (text.find('\0') != std::string::npos) {
    throw std::invalid_argument("Connection string contains an embedded NUL byte");
  }
  std::map<std::string, std::string> fields;
  std::size_t position = 0;
  while (position < text.size()) {
    const auto separator = text.find(';', position);
    const auto equals = text.find('=', position);
    if (equals == std::string::npos || (separator != std::string::npos && separator < equals)) {
      if (separator == std::string::npos) break;
      position = separator + 1;
      continue;
    }
    const auto key = trim(text.substr(position, equals - position));
    position = equals + 1;
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) {
      ++position;
    }
    std::string value;
    if (position < text.size() && text[position] == '{') {
      ++position;
      bool closed = false;
      while (position < text.size()) {
        const char current = text[position++];
        if (current != '}') {
          value.push_back(current);
        } else if (position < text.size() && text[position] == '}') {
          value.push_back('}');
          ++position;
        } else {
          closed = true;
          break;
        }
      }
      if (!closed) throw std::invalid_argument("Unterminated braced connection string value");
      while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) {
        ++position;
      }
      if (position < text.size() && text[position] != ';') {
        throw std::invalid_argument("Unexpected text after braced connection string value");
      }
    } else {
      const auto end = text.find(';', position);
      value = trim(text.substr(position, end == std::string::npos ? end : end - position));
      position = end == std::string::npos ? text.size() : end;
    }
    if (!key.empty()) fields.try_emplace(to_upper(key), std::move(value));
    if (position < text.size()) ++position;
  }
  return fields;
}

}  // namespace odbcpp::test::connection_string
#else
#include "odbc/connection_string.h"

namespace odbcpp::test::connection_string {

inline std::map<std::string, std::string> parse(const std::string& text) {
  return rs::odbc::ConnectionString::parse(text);
}
inline std::string trim(const std::string& text) { return rs::odbc::ConnectionString::trim(text); }
inline std::string to_upper(const std::string& text) {
  return rs::odbc::ConnectionString::to_upper(text);
}

}  // namespace odbcpp::test::connection_string
#endif
