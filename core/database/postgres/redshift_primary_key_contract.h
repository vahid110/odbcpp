#pragma once

#include "core/database/query_result.h"
#include <optional>
#include <string>

namespace rs::core::database::postgres {

// Offline value model only: no SQL rendering, escaping, execution or capability
// inference. All three identifiers are required exact names; no default catalog.
struct RedshiftPrimaryKeyCommandPlan {
  std::string database;
  std::string schema;
  std::string table;
};

BackendResult<RedshiftPrimaryKeyCommandPlan> redshift_primary_key_plan(
    std::optional<std::string> database, std::optional<std::string> schema,
    std::optional<std::string> table);

// Pure owning normalization of one exact-table SHOW PRIMARY KEYS result.
// Pinned official source 56d35297f9bee0cc31c0148581c87ca455639a39:
// rsMetadataServerProxyHelper.cpp:743-810 binds database_name, schema_name,
// table_name, column_name, pk_name as SQL_C_CHAR and key_seq as SQL_C_SSHORT;
// rslibpq.c:4151-4213 emits six ODBC fields, preserving input row order.
// Those bindings do NOT prove native SHOW OIDs or widths. Accepted known
// Char/VarChar/LongVarChar and SmallInt/Integer are this candidate's normalized
// input contract, not live descriptor qualification. Output text size 0 remains
// unknown. Nullable pk_name is preserved; required identity/sequence NULLs,
// inconsistent names, duplicate columns/keys, and noncontiguous sequences block.
// Additional results never produce partial success; native failures are retained.
BackendResult<QueryResult> normalize_redshift_primary_keys(
    const RedshiftPrimaryKeyCommandPlan& plan,
    BackendResult<QueryResult> input);

}  // namespace rs::core::database::postgres
