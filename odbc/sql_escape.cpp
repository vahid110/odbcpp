#include "sql_escape.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace rs::odbc {
namespace {

std::string_view trim(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return value;
}

bool starts_with_word(std::string_view value, std::string_view word) {
  if (value.size() < word.size()) return false;
  for (std::size_t i = 0; i < word.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(word[i]))) {
      return false;
    }
  }
  return value.size() == word.size() ||
      std::isspace(static_cast<unsigned char>(value[word.size()])) ||
      value[word.size()] == '(';
}

std::optional<int> decimal(std::string_view value) {
  int parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                      parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

bool leap_year(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool valid_date(std::string_view value) {
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
  const auto year = decimal(value.substr(0, 4));
  const auto month = decimal(value.substr(5, 2));
  const auto day = decimal(value.substr(8, 2));
  if (!year || !month || !day || *year < 1 || *month < 1 || *month > 12) {
    return false;
  }
  constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};
  const int maximum = days[static_cast<std::size_t>(*month - 1)] +
      (*month == 2 && leap_year(*year) ? 1 : 0);
  return *day >= 1 && *day <= maximum;
}

bool valid_time(std::string_view value) {
  if (value.size() < 8 || value[2] != ':' || value[5] != ':') return false;
  const auto hour = decimal(value.substr(0, 2));
  const auto minute = decimal(value.substr(3, 2));
  const auto second = decimal(value.substr(6, 2));
  if (!hour || !minute || !second || *hour > 23 || *minute > 59 ||
      *second > 59) {
    return false;
  }
  if (value.size() == 8) return true;
  if (value[8] != '.' || value.size() == 9) return false;
  return std::all_of(value.begin() + 9, value.end(), [](unsigned char c) {
    return std::isdigit(c);
  });
}

std::optional<std::string_view> quoted_value(std::string_view value) {
  value = trim(value);
  if (value.size() < 2 || value.front() != '\'' || value.back() != '\'') {
    return std::nullopt;
  }
  return value.substr(1, value.size() - 2);
}

std::optional<std::string_view> dollar_tag_at(std::string_view sql,
                                               std::size_t position) {
  if (sql[position] != '$') return std::nullopt;
  const auto end = sql.find('$', position + 1);
  if (end == std::string_view::npos) return std::nullopt;
  const auto name = sql.substr(position + 1, end - position - 1);
  if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
      })) {
    return std::nullopt;
  }
  return sql.substr(position, end - position + 1);
}

std::optional<std::size_t> quoted_end(std::string_view sql,
                                      std::size_t position) {
  const char quote = sql[position];
  for (std::size_t i = position + 1; i < sql.size(); ++i) {
    if (sql[i] != quote) continue;
    if (i + 1 < sql.size() && sql[i + 1] == quote) {
      ++i;
      continue;
    }
    return i + 1;
  }
  return std::nullopt;
}

std::optional<std::size_t> comment_end(std::string_view sql,
                                       std::size_t position) {
  if (sql.substr(position, 2) == "--") {
    const auto end = sql.find('\n', position + 2);
    return end == std::string_view::npos ? sql.size() : end + 1;
  }
  if (sql.substr(position, 2) == "/*") {
    const auto end = sql.find("*/", position + 2);
    return end == std::string_view::npos
        ? std::optional<std::size_t>{}
        : std::optional<std::size_t>{end + 2};
  }
  return std::nullopt;
}

std::optional<std::size_t> dollar_end(std::string_view sql,
                                      std::size_t position) {
  const auto tag = dollar_tag_at(sql, position);
  if (!tag) return std::nullopt;
  const auto end = sql.find(*tag, position + tag->size());
  return end == std::string_view::npos
      ? std::optional<std::size_t>{}
      : std::optional<std::size_t>{end + tag->size()};
}

std::optional<std::size_t> escape_end(std::string_view sql,
                                      std::size_t position) {
  int depth = 1;
  for (std::size_t i = position + 1; i < sql.size();) {
    if (sql[i] == '\'' || sql[i] == '"') {
      const auto end = quoted_end(sql, i);
      if (!end) return std::nullopt;
      i = *end;
      continue;
    }
    if (sql.substr(i, 2) == "--" || sql.substr(i, 2) == "/*") {
      const auto end = comment_end(sql, i);
      if (!end) return std::nullopt;
      i = *end;
      continue;
    }
    if (sql[i] == '$') {
      const auto tag = dollar_tag_at(sql, i);
      if (tag) {
        const auto end = dollar_end(sql, i);
        if (!end) return std::nullopt;
        i = *end;
        continue;
      }
    }
    if (sql[i] == '{') ++depth;
    if (sql[i] == '}' && --depth == 0) return i;
    ++i;
  }
  return std::nullopt;
}

SqlEscapeResult failure(SqlEscapeError error, std::string message) {
  return {{}, error, std::move(message)};
}

SqlEscapeResult translate_fragment(std::string_view sql);

SqlEscapeResult translate_datetime(std::string_view body,
                                   std::string_view keyword,
                                   std::string_view native_keyword) {
  const auto literal = quoted_value(body.substr(keyword.size()));
  if (!literal) {
    return failure(SqlEscapeError::InvalidDatetime,
                   "ODBC datetime escape requires a quoted literal");
  }
  bool valid = false;
  if (keyword == "d") valid = valid_date(*literal);
  if (keyword == "t") valid = valid_time(*literal);
  if (keyword == "ts") {
    valid = literal->size() > 11 && (*literal)[10] == ' ' &&
        valid_date(literal->substr(0, 10)) && valid_time(literal->substr(11));
  }
  if (!valid) {
    return failure(SqlEscapeError::InvalidDatetime,
                   "ODBC datetime escape contains an invalid value");
  }
  return {std::string(native_keyword) + " '" + std::string(*literal) + "'",
          SqlEscapeError::None, {}};
}

SqlEscapeResult translate_escape(std::string_view body) {
  body = trim(body);
  if (starts_with_word(body, "ts")) {
    return translate_datetime(body, "ts", "TIMESTAMP");
  }
  if (starts_with_word(body, "d")) {
    return translate_datetime(body, "d", "DATE");
  }
  if (starts_with_word(body, "t")) {
    return translate_datetime(body, "t", "TIME");
  }
  if (starts_with_word(body, "fn")) {
    auto translated = translate_fragment(trim(body.substr(2)));
    if (!translated) return translated;
    auto function = trim(translated.sql);
    const auto open = function.find('(');
    if (open != std::string_view::npos) {
      const auto name = trim(function.substr(0, open));
      std::string replacement;
      if (starts_with_word(name, "UCASE")) replacement = "UPPER";
      if (starts_with_word(name, "LCASE")) replacement = "LOWER";
      if (starts_with_word(name, "IFNULL")) replacement = "COALESCE";
      if (starts_with_word(name, "CURDATE")) replacement = "CURRENT_DATE";
      if (starts_with_word(name, "CURTIME")) replacement = "CURRENT_TIME";
      if (starts_with_word(name, "NOW")) replacement = "CURRENT_TIMESTAMP";
      if (!replacement.empty()) {
        const bool keyword_function = replacement.starts_with("CURRENT_");
        const auto close = function.find_last_not_of(" \t\r\n");
        const bool empty_arguments = close != std::string_view::npos &&
            function[close] == ')' && trim(function.substr(
                open + 1, close - open - 1)).empty();
        if (keyword_function && empty_arguments) {
          translated.sql = replacement;
        } else {
          translated.sql.replace(0, open, replacement);
        }
      }
    }
    return translated;
  }
  if (starts_with_word(body, "oj")) {
    return translate_fragment(trim(body.substr(2)));
  }
  if (starts_with_word(body, "escape")) {
    const auto literal = quoted_value(body.substr(6));
    if (!literal || literal->size() != 1) {
      return failure(SqlEscapeError::InvalidSyntax,
                     "ODBC LIKE escape must contain one character");
    }
    return {"ESCAPE '" + std::string(*literal) + "'",
            SqlEscapeError::None, {}};
  }
  if (starts_with_word(body, "call")) {
    auto translated = translate_fragment(trim(body.substr(4)));
    if (!translated) return translated;
    translated.sql.insert(0, "CALL ");
    return translated;
  }
  if (body.starts_with("?")) {
    return failure(SqlEscapeError::Unsupported,
                   "ODBC function-return procedure calls are not supported");
  }
  // Unknown braces may be native PostgreSQL syntax. ODBC requires drivers to
  // pass grammar they do not recognize without modification.
  return {"{" + std::string(body) + "}", SqlEscapeError::None, {}};
}

SqlEscapeResult translate_fragment(std::string_view sql) {
  std::string output;
  output.reserve(sql.size());
  for (std::size_t i = 0; i < sql.size();) {
    std::optional<std::size_t> skipped;
    if (sql[i] == '\'' || sql[i] == '"') skipped = quoted_end(sql, i);
    if (!skipped &&
        (sql.substr(i, 2) == "--" || sql.substr(i, 2) == "/*")) {
      skipped = comment_end(sql, i);
    }
    if (!skipped && sql[i] == '$' && dollar_tag_at(sql, i)) {
      skipped = dollar_end(sql, i);
    }
    if (skipped) {
      output.append(sql.substr(i, *skipped - i));
      i = *skipped;
      continue;
    }
    if (sql[i] != '{') {
      output.push_back(sql[i++]);
      continue;
    }
    const auto end = escape_end(sql, i);
    if (!end) {
      return failure(SqlEscapeError::InvalidSyntax,
                     "ODBC escape clause has no closing brace");
    }
    auto translated = translate_escape(sql.substr(i + 1, *end - i - 1));
    if (!translated) return translated;
    output += translated.sql;
    i = *end + 1;
  }
  return {std::move(output), SqlEscapeError::None, {}};
}

} // namespace

SqlEscapeResult translate_odbc_sql(std::string_view sql) {
  return translate_fragment(sql);
}

} // namespace rs::odbc
