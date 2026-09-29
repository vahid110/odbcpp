#pragma once
#include "core/database/statement_kind.h"
#include <string_view>

namespace rs::core::database::postgres {
StatementKind classify_command_tag(std::string_view tag);
}
