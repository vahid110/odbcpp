#pragma once

#include <array>
#include <cstddef>

namespace rs::core::database {

enum class TransactionAction { Begin, Commit, Rollback };
enum class TransactionIsolation { ReadUncommitted, ReadCommitted, RepeatableRead, Serializable };
inline constexpr std::array transaction_isolations{
    TransactionIsolation::ReadUncommitted, TransactionIsolation::ReadCommitted,
    TransactionIsolation::RepeatableRead, TransactionIsolation::Serializable};

struct TransactionCapabilities {
  bool supported{false};
  bool transactional_ddl{false};
  TransactionIsolation default_isolation{TransactionIsolation::ReadCommitted};
  std::array<bool, 4> isolation_levels{};

  bool supports(TransactionIsolation level) const {
    const auto index = static_cast<std::size_t>(level);
    return supported && index < isolation_levels.size() && isolation_levels[index];
  }
};

} // namespace rs::core::database
