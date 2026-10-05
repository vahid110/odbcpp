#pragma once

#include "odbcpp/database/sql_translation.h"

namespace rs::core::database::postgres {

// Translates ODBC escapes using PostgreSQL syntax and lexical rules.
SqlTranslationResult translate_odbc_sql(std::string_view sql);

} // namespace rs::core::database::postgres
