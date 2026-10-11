#pragma once

#include "odbcpp/database/sql_translation.h"

namespace rs::core::database::postgres {

enum class SqlDialectProfile { PostgreSql, Redshift };

// Native spelling is backend-owned; default preserves PostgreSQL source callers.
SqlTranslationResult translate_odbc_sql(std::string_view sql,
    SqlDialectProfile profile = SqlDialectProfile::PostgreSql);

} // namespace rs::core::database::postgres
