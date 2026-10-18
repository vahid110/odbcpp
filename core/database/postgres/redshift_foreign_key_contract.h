#pragma once
#include "odbcpp/database/query_result.h"
#include "odbcpp/util/deadline.h"
#include <optional>
#include <string>

namespace rs::core::database::postgres {
struct RedshiftForeignKeyTable {
  std::string database, schema, table;
};
enum class RedshiftForeignKeyDirection { Imported, Exported, Both };
// Owning values only: no SQL, escaping, defaults or capability inference.
struct RedshiftForeignKeyCommandPlan {
  RedshiftForeignKeyDirection direction;
  std::optional<RedshiftForeignKeyTable> primary;
  std::optional<RedshiftForeignKeyTable> foreign;
};
BackendResult<RedshiftForeignKeyCommandPlan> redshift_foreign_key_plan(
    RedshiftForeignKeyDirection direction,
    std::optional<RedshiftForeignKeyTable> primary,
    std::optional<RedshiftForeignKeyTable> foreign);

// Offline contract, pinned official source 56d35297f9bee0cc31c0148581c87ca455639a39:
// rsMetadataServerProxyHelper.cpp:864-939 binds ten strings/four SMALLINTs;
// rslibpq.c:4160-4225 defines output order and NULL defaults (3,3,7).
// Known text and SmallInt/Integer families are candidate normalized inputs,
// NOT native SHOW OID/width proof. Output text widths remain unknown (0).
// Conservative policies beyond upstream: reject NULL required identity/sequence,
// NULL FK_NAME (ambiguous grouping), malformed/noncontiguous sequences and invalid
// action/deferrability enums. Nullable PK_NAME is retained and consistent per group.
// Validate all rows before Both filtering; preserve source row order, no partial
// success, no synthesized health/retry authority or SQL execution.
BackendResult<QueryResult> normalize_redshift_foreign_keys(
    const RedshiftForeignKeyCommandPlan& plan, BackendResult<QueryResult> input);
}  // namespace rs::core::database::postgres

namespace rs::core::database::postgres {
class RedshiftRoutineBudget;
struct RedshiftRoutineUsage;
std::optional<RedshiftRoutineUsage> redshift_foreign_key_output_usage(const QueryResult& source);
struct RedshiftPrimaryKeyCommandPlan;
// SHOW FOREIGN KEYS includes UNIQUE references. ODBC publishes PRIMARY-only
// groups; compare the complete normalized group to actual SHOW PRIMARY KEYS.
BackendResult<void> redshift_filter_primary_references(QueryResult& foreign_keys,
    const RedshiftPrimaryKeyCommandPlan& table, const QueryResult& primary_keys,
    RedshiftRoutineBudget& budget, rs::util::Deadline deadline);
// Bounded native current-database identity/foreign-key publication. The strict
// value plan and pure normalizer above retain their original contracts.
BackendResult<std::string> redshift_foreign_key_database(BackendResult<QueryResult> input,
    RedshiftRoutineBudget& budget, rs::util::Deadline deadline);
BackendResult<QueryResult> redshift_foreign_key_part(const RedshiftForeignKeyCommandPlan& plan,
    BackendResult<QueryResult> input, RedshiftRoutineBudget& budget, rs::util::Deadline deadline);
}
