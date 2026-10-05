// Backend-private compile-only probe. First include resolves solely in staging.
#include "core/database/mysql/mysql_session.h"

namespace {
using rs::core::database::mysql::MySqlSession;
static_assert(std::is_base_of_v<rs::core::database::IDatabaseConnection, MySqlSession>);
static_assert(std::is_base_of_v<rs::core::database::ITransactionSession, MySqlSession>);
static_assert(!std::is_default_constructible_v<MySqlSession>);
static_assert(!std::is_copy_constructible_v<MySqlSession>);
static_assert(!std::is_copy_assignable_v<MySqlSession>);
static_assert(std::tuple_size_v<decltype(MySqlSession::CompletePrepareObservation::parameters)> == 3);
static_assert(std::tuple_size_v<decltype(MySqlSession::CompletePrepareObservation::result_types)> == 4);
static_assert(requires(MySqlSession& session, std::string_view sql, rs::util::Deadline deadline) {
  session.observe_combined_prepare(sql, deadline,
      MySqlSession::PrepareObservationPolicy::DateDatetimeQ6Decimal65Q30ThreeByFour);
});
}  // namespace
