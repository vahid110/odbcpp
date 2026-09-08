#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rs::core::database {

struct ResultColumnMetadata {
  std::string name;
  std::uint32_t table_id{0};
  std::int16_t table_column{0};
  std::uint32_t type_id{0};
  std::int16_t type_size{-1};
  std::int32_t type_modifier{-1};
  std::int16_t format_code{0};
};

struct QueryResult {
  std::vector<std::vector<std::string>> rows;
  std::vector<ResultColumnMetadata> columns;
  std::vector<std::uint32_t> parameter_type_ids;
  std::string command_tag;
  std::size_t affected_rows{0};
};

} // namespace rs::core::database
