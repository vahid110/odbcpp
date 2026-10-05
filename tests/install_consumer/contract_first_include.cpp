#include "core/database/query_result.h"
#include <type_traits>
static_assert(std::is_copy_constructible_v<rs::core::database::QueryResult>);
static_assert(std::is_move_constructible_v<rs::core::database::QueryResult>);
