#include "pg_sql_dialect.h"
#include "odbcpp/util/utf8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace rs::core::database::postgres {
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
  const auto digits = [](std::string_view part) {
    return std::all_of(part.begin(), part.end(), [](unsigned char c) {
      return std::isdigit(c);
    });
  };
  if (!digits(value.substr(0, 2)) || !digits(value.substr(3, 2)) ||
      !digits(value.substr(6, 2))) {
    return false;
  }
  const auto hour = decimal(value.substr(0, 2));
  const auto minute = decimal(value.substr(3, 2));
  const auto second = decimal(value.substr(6, 2));
  if (!hour || !minute || !second || *hour > 23 || *minute > 59 ||
      *second > 59) {
    return false;
  }
  if (value.size() == 8) return true;
  if (value[8] != '.' || value.size() == 9) return false;
  return digits(value.substr(9));
}

std::optional<std::string_view> quoted_value(std::string_view value) {
  value = trim(value);
  if (value.size() < 2 || value.front() != '\'' || value.back() != '\'') {
    return std::nullopt;
  }
  return value.substr(1, value.size() - 2);
}

bool identifier_continue(unsigned char ch) {
  return std::isalnum(ch) || ch == '_' || ch == '$' || ch >= 0x80;
}

std::optional<std::string_view> dollar_tag_at(std::string_view sql,
                                               std::size_t position) {
  if (sql[position] != '$' ||
      (position > 0 && identifier_continue(
          static_cast<unsigned char>(sql[position - 1])))) {
    return std::nullopt;
  }
  const auto end = sql.find('$', position + 1);
  if (end == std::string_view::npos) return std::nullopt;
  const auto name = sql.substr(position + 1, end - position - 1);
  if (!name.empty() &&
      !std::isalpha(static_cast<unsigned char>(name.front())) &&
      name.front() != '_') {
    return std::nullopt;
  }
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
  const bool escape_string = quote == '\'' && position > 0 &&
      (sql[position - 1] == 'E' || sql[position - 1] == 'e') &&
      (position == 1 || !identifier_continue(
          static_cast<unsigned char>(sql[position - 2])));
  for (std::size_t i = position + 1; i < sql.size(); ++i) {
    if (escape_string && sql[i] == '\\' && i + 1 < sql.size()) {
      ++i;
      continue;
    }
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
    const auto end = sql.find_first_of("\r\n", position + 2);
    return end == std::string_view::npos ? sql.size() : end + 1;
  }
  if (sql.substr(position, 2) == "/*") {
    std::size_t depth = 1;
    for (std::size_t i = position + 2; i + 1 < sql.size();) {
      if (sql.substr(i, 2) == "/*") {
        ++depth;
        i += 2;
      } else if (sql.substr(i, 2) == "*/") {
        if (--depth == 0) return i + 2;
        i += 2;
      } else {
        ++i;
      }
    }
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

SqlTranslationResult failure(SqlTranslationError error, std::string message) {
  return {{}, error, std::move(message)};
}

bool ascii_word_equal(std::string_view value, std::string_view word) {
  if (value.size() != word.size()) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto ch = value[i];
    const auto folded = ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 'a' + 'A') : ch;
    if (folded != word[i]) return false;
  }
  return true;
}

std::optional<std::size_t> skip_trivia(std::string_view value, std::size_t position) {
  while (position < value.size()) {
    const auto ch = value[position];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v') {
      ++position;
    } else if (value.substr(position, 2) == "/*" || value.substr(position, 2) == "--") {
      const auto end = comment_end(value, position);
      if (!end) return std::nullopt;
      position = *end;
    } else {
      break;
    }
  }
  return position;
}

// Extract only the outer call's argument slices. Expressions remain native
// authority; skipping lexical bodies and nested parentheses preserves commas
// and parameter order without inventing an expression parser.
SqlTranslationResult translate_locate(std::string_view function, std::size_t name_end) {
  const auto open = skip_trivia(function, name_end);
  if (!open || *open == function.size() || function[*open] != '(')
    return failure(SqlTranslationError::InvalidSyntax, "ODBC LOCATE requires an argument list");
  std::array<std::string_view,3> arguments{};
  std::size_t count = 0;
  std::size_t begin = *open + 1;
  std::size_t depth = 1;
  for (std::size_t i = begin; i < function.size();) {
    std::optional<std::size_t> skipped;
    if (function[i] == '\'' || function[i] == '"') {
      skipped = quoted_end(function, i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed LOCATE quoted argument");
    } else if (function.substr(i,2) == "--" || function.substr(i,2) == "/*") {
      skipped = comment_end(function,i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed LOCATE comment");
    } else if (function[i] == '$' && dollar_tag_at(function,i)) {
      skipped = dollar_end(function,i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed LOCATE dollar argument");
    }
    if (skipped) { i = *skipped; continue; }
    if (function[i] == '(') { ++depth; ++i; continue; }
    const bool close = function[i] == ')' && depth == 1;
    if ((function[i] == ',' && depth == 1) || close) {
      const auto argument = function.substr(begin,i-begin);
      const auto first = skip_trivia(argument,0);
      if (!first || *first == argument.size() || count == arguments.size())
        return failure(SqlTranslationError::InvalidSyntax, "Invalid LOCATE argument count or empty argument");
      arguments[count++] = argument;
      if (close) {
        const auto end = skip_trivia(function,i+1);
        if (!end || *end != function.size() || count < 2)
          return failure(SqlTranslationError::InvalidSyntax, "Invalid LOCATE call structure");
        if (count == 3)
          return failure(SqlTranslationError::Unsupported, "LOCATE start-position argument is not supported");
        // Do not trim argument tails: the newline terminating a -- comment
        // must remain before generated IN or ')'. Every expression occurs once.
        std::string result{"POSITION("}; result.append(arguments[0]);
        result += " IN "; result.append(arguments[1]); result += ')';
        return {std::move(result),SqlTranslationError::None,{}};
      }
      begin = ++i; continue;
    }
    if (function[i] == ')') --depth;
    ++i;
  }
  return failure(SqlTranslationError::InvalidSyntax, "Unclosed LOCATE call");
}

SqlTranslationResult translate_length(std::string_view function, std::size_t name_end) {
  const auto open = skip_trivia(function, name_end);
  if (!open || *open == function.size() || function[*open] != '(')
    return failure(SqlTranslationError::InvalidSyntax, "ODBC LENGTH requires an argument list");
  std::array<std::string_view,1> arguments{};
  std::size_t count = 0;
  std::size_t begin = *open + 1;
  std::size_t depth = 1;
  for (std::size_t i = begin; i < function.size();) {
    std::optional<std::size_t> skipped;
    if (function[i] == '\'' || function[i] == '"') {
      skipped = quoted_end(function, i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed LENGTH quoted argument");
    } else if (function.substr(i,2) == "--" || function.substr(i,2) == "/*") {
      skipped = comment_end(function,i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed LENGTH comment");
    } else if (function[i] == '$' && dollar_tag_at(function,i)) {
      skipped = dollar_end(function,i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed LENGTH dollar argument");
    }
    if (skipped) { i = *skipped; continue; }
    if (function[i] == '(') { ++depth; ++i; continue; }
    const bool close = function[i] == ')' && depth == 1;
    if ((function[i] == ',' && depth == 1) || close) {
      const auto argument = function.substr(begin,i-begin);
      const auto first = skip_trivia(argument,0);
      if (!first || *first == argument.size() || count == arguments.size())
        return failure(SqlTranslationError::InvalidSyntax, "Invalid LENGTH argument count or empty argument");
      arguments[count++] = argument;
      if (close) {
        const auto end = skip_trivia(function,i+1);
        if (!end || *end != function.size() || count != 1)
          return failure(SqlTranslationError::InvalidSyntax, "Invalid LENGTH call structure");
        // Preserve the argument tail newline: a line comment must end before
        // the trim literal and generated closers. The expression occurs exactly once.
        std::string result{"LENGTH(RTRIM("}; result.append(arguments[0]);
        // Specify U+0020 so native default trimming cannot discard a TAB.
        result += ", ' '))";
        return {std::move(result),SqlTranslationError::None,{}};
      }
      begin = ++i; continue;
    }
    if (function[i] == ')') --depth;
    ++i;
  }
  return failure(SqlTranslationError::InvalidSyntax, "Unclosed LENGTH call");
}

SqlTranslationResult translate_calendar_component(std::string_view function, std::size_t name_end,
                                                   std::string_view part) {
  const auto open = skip_trivia(function, name_end);
  if (!open || *open == function.size() || function[*open] != '(')
    return failure(SqlTranslationError::InvalidSyntax, "ODBC calendar component requires an argument list");
  std::array<std::string_view,1> arguments{};
  std::size_t count = 0;
  std::size_t begin = *open + 1;
  std::size_t depth = 1;
  for (std::size_t i = begin; i < function.size();) {
    std::optional<std::size_t> skipped;
    if (function[i] == '\'' || function[i] == '"') {
      skipped = quoted_end(function, i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed calendar component quoted argument");
    } else if (function.substr(i,2) == "--" || function.substr(i,2) == "/*") {
      skipped = comment_end(function,i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed calendar component comment");
    } else if (function[i] == '$' && dollar_tag_at(function,i)) {
      skipped = dollar_end(function,i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed calendar component dollar argument");
    }
    if (skipped) { i = *skipped; continue; }
    if (function[i] == '(') { ++depth; ++i; continue; }
    const bool close = function[i] == ')' && depth == 1;
    if ((function[i] == ',' && depth == 1) || close) {
      const auto argument = function.substr(begin,i-begin);
      const auto first = skip_trivia(argument,0);
      if (!first || *first == argument.size() || count == arguments.size())
        return failure(SqlTranslationError::InvalidSyntax, "Invalid calendar component argument count or empty argument");
      arguments[count++] = argument;
      if (close) {
        const auto end = skip_trivia(function,i+1);
        if (!end || *end != function.size() || count != 1)
          return failure(SqlTranslationError::InvalidSyntax, "Invalid calendar component call structure");
        // Preserve the argument tail newline: a line comment must end before
        // both generated closers. The native expression occurs exactly once.
        std::string result{"CAST(DATE_PART("}; result.append(part);
        result += ", "; result.append(arguments[0]); result += ") AS INTEGER)";
        return {std::move(result),SqlTranslationError::None,{}};
      }
      begin = ++i; continue;
    }
    if (function[i] == ')') --depth;
    ++i;
  }
  return failure(SqlTranslationError::InvalidSyntax, "Unclosed calendar component call");
}

SqlTranslationResult translate_clock_component(std::string_view function, std::size_t name_end,
                                                   std::string_view part) {
  const auto open = skip_trivia(function, name_end);
  if (!open || *open == function.size() || function[*open] != '(')
    return failure(SqlTranslationError::InvalidSyntax, "ODBC clock component requires an argument list");
  std::array<std::string_view,1> arguments{};
  std::size_t count = 0;
  std::size_t begin = *open + 1;
  std::size_t depth = 1;
  for (std::size_t i = begin; i < function.size();) {
    std::optional<std::size_t> skipped;
    if (function[i] == '\'' || function[i] == '"') {
      skipped = quoted_end(function, i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed clock component quoted argument");
    } else if (function.substr(i,2) == "--" || function.substr(i,2) == "/*") {
      skipped = comment_end(function,i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed clock component comment");
    } else if (function[i] == '$' && dollar_tag_at(function,i)) {
      skipped = dollar_end(function,i);
      if (!skipped) return failure(SqlTranslationError::InvalidSyntax, "Unclosed clock component dollar argument");
    }
    if (skipped) { i = *skipped; continue; }
    if (function[i] == '(') { ++depth; ++i; continue; }
    const bool close = function[i] == ')' && depth == 1;
    if ((function[i] == ',' && depth == 1) || close) {
      const auto argument = function.substr(begin,i-begin);
      const auto first = skip_trivia(argument,0);
      if (!first || *first == argument.size() || count == arguments.size())
        return failure(SqlTranslationError::InvalidSyntax, "Invalid clock component argument count or empty argument");
      arguments[count++] = argument;
      if (close) {
        const auto end = skip_trivia(function,i+1);
        if (!end || *end != function.size() || count != 1)
          return failure(SqlTranslationError::InvalidSyntax, "Invalid clock component call structure");
        // Preserve the argument tail newline: a line comment must end before
        // all generated closers. The native expression occurs exactly once.
        // ODBC clock fields are whole integers; truncate fractional seconds
        // before casting so 59.999999 cannot round into the next minute.
        std::string result{"CAST(FLOOR(EXTRACT("}; result.append(part);
        result += " FROM "; result.append(arguments[0]); result += ")) AS INTEGER)";
        return {std::move(result),SqlTranslationError::None,{}};
      }
      begin = ++i; continue;
    }
    if (function[i] == ')') --depth;
    ++i;
  }
  return failure(SqlTranslationError::InvalidSyntax, "Unclosed clock component call");
}

SqlTranslationResult translate_fragment(std::string_view sql, SqlDialectProfile profile);

SqlTranslationResult translate_datetime(std::string_view body,
                                   std::string_view keyword,
                                   std::string_view native_keyword) {
  const auto literal = quoted_value(body.substr(keyword.size()));
  if (!literal) {
    return failure(SqlTranslationError::InvalidDatetime,
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
    return failure(SqlTranslationError::InvalidDatetime,
                   "ODBC datetime escape contains an invalid value");
  }
  return {std::string(native_keyword) + " '" + std::string(*literal) + "'",
          SqlTranslationError::None, {}};
}

SqlTranslationResult translate_escape(std::string_view body, SqlDialectProfile profile) {
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
    auto translated = translate_fragment(trim(body.substr(2)), profile);
    if (!translated) return translated;
    auto function = trim(translated.sql);
    if (profile == SqlDialectProfile::Redshift) {
      const auto start = skip_trivia(function, 0);
      if (start) {
        auto name_end = *start;
        while (name_end < function.size() && identifier_continue(
            static_cast<unsigned char>(function[name_end]))) ++name_end;
        const auto name = function.substr(*start, name_end - *start);
        if (ascii_word_equal(name, "LOCATE")) return translate_locate(function,name_end);
        if (ascii_word_equal(name, "LENGTH")) return translate_length(function,name_end);
        if (ascii_word_equal(name, "YEAR")) return translate_calendar_component(function,name_end,"year");
        if (ascii_word_equal(name, "MONTH")) return translate_calendar_component(function,name_end,"month");
        if (ascii_word_equal(name, "DAYOFMONTH")) return translate_calendar_component(function,name_end,"day");
        if (ascii_word_equal(name, "HOUR")) return translate_clock_component(function,name_end,"hour");
        if (ascii_word_equal(name, "MINUTE")) return translate_clock_component(function,name_end,"minute");
        if (ascii_word_equal(name, "SECOND")) return translate_clock_component(function,name_end,"second");
        if (ascii_word_equal(name, "NOW") || ascii_word_equal(name, "CURTIME")) {
          const auto open_at = skip_trivia(function, name_end);
          const auto close_at = open_at && *open_at < function.size() && function[*open_at] == '('
              ? skip_trivia(function, *open_at + 1) : std::nullopt;
          const auto end_at = close_at && *close_at < function.size() && function[*close_at] == ')'
              ? skip_trivia(function, *close_at + 1) : std::nullopt;
          if (!end_at || *end_at != function.size()) {
            return failure(SqlTranslationError::InvalidSyntax,
                "ODBC current-time escape requires exactly zero arguments");
          }
          return {ascii_word_equal(name, "NOW") ? "GETDATE()" : "CAST(GETDATE() AS TIME)",
              SqlTranslationError::None, {}};
        }
      }
    }
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
    return translate_fragment(trim(body.substr(2)), profile);
  }
  if (starts_with_word(body, "escape")) {
    const auto literal = quoted_value(body.substr(6));
    if (!literal || (*literal != "''" &&
                     rs::util::utf8_code_point_count(*literal) != 1)) {
      return failure(SqlTranslationError::InvalidSyntax,
                     "ODBC LIKE escape must contain one character");
    }
    return {"ESCAPE '" + std::string(*literal) + "'",
            SqlTranslationError::None, {}};
  }
  if (starts_with_word(body, "call")) {
    auto translated = translate_fragment(trim(body.substr(4)), profile);
    if (!translated) return translated;
    translated.sql.insert(0, "CALL ");
    return translated;
  }
  if (body.starts_with("?")) {
    return failure(SqlTranslationError::Unsupported,
                   "ODBC function-return procedure calls are not supported");
  }
  // Unknown braces may be native PostgreSQL syntax. ODBC requires drivers to
  // pass grammar they do not recognize without modification.
  return {"{" + std::string(body) + "}", SqlTranslationError::None, {}};
}

SqlTranslationResult translate_fragment(std::string_view sql, SqlDialectProfile profile) {
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
      return failure(SqlTranslationError::InvalidSyntax,
                     "ODBC escape clause has no closing brace");
    }
    auto translated = translate_escape(sql.substr(i + 1, *end - i - 1), profile);
    if (!translated) return translated;
    output += translated.sql;
    i = *end + 1;
  }
  return {std::move(output), SqlTranslationError::None, {}};
}

} // namespace

SqlTranslationResult translate_odbc_sql(std::string_view sql, SqlDialectProfile profile) {
  return translate_fragment(sql, profile);
}

} // namespace rs::core::database::postgres
