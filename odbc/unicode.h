#pragma once

#include "odbc_types.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rs::odbc {

std::optional<std::string> wide_to_utf8(std::span<const SQLWCHAR> input);
std::optional<std::string> sqlwchar_to_utf8(
    const SQLWCHAR* input, SQLINTEGER length);
std::optional<std::vector<SQLWCHAR>> utf8_to_wide(std::string_view input);

} // namespace rs::odbc
