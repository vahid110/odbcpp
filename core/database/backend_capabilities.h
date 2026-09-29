#pragma once
#include <cstdint>
#include <string_view>

namespace rs::core::database {

enum class IdentifierCase { Sensitive, Upper, Lower, Mixed };
enum class NullCollation { High, Low, Start, End };
enum class CorrelationNames { None, Different, Any };
enum class GroupBySupport { None, EqualsSelect, ContainsSelect, Unrelated };

// No ODBC identifiers or native protocol IDs. Zero limits mean unknown/no
// declared limit. Strings must remain valid for the connection lifetime.
// These describe the exposed backend profile, not every server feature.
struct BackendCapabilities {
  std::string_view dbms_name{};
  std::string_view identifier_quote{};
  std::string_view catalog_separator{};
  std::string_view catalog_term{};
  std::string_view schema_term{};
  std::string_view table_term{};
  std::string_view procedure_term{};
  std::string_view pattern_escape{};
  std::uint16_t max_identifier_length{0};
  IdentifierCase identifier_case{IdentifierCase::Sensitive};
  IdentifierCase quoted_identifier_case{IdentifierCase::Sensitive};
  NullCollation null_collation{NullCollation::High};
  CorrelationNames correlation_names{CorrelationNames::None};
  GroupBySupport group_by{GroupBySupport::None};
  bool catalog_at_start{true};
  bool catalog_names{false};
  bool column_aliases{false};
  bool describe_parameters{false};
  bool order_by_expressions{false};
  bool order_by_requires_select{true};
  bool read_only{true};
  bool integrity{false};
  bool like_escape{false};
  bool outer_joins{false};
  bool procedures{false};
  bool concat_null_yields_null{true};
  bool non_nullable_columns{false};
  bool create_index{false};
  bool drop_index{false};
  bool insert_literals{false};
  bool insert_searched{false};
  bool select_into{false};
  bool sql92_entry{false};
  bool union_distinct{false};
  bool union_all{false};
  bool schema_in_dml{false};
  bool schema_in_procedures{false};
  bool schema_in_table_definitions{false};
  bool schema_in_index_definitions{false};
  bool schema_in_privileges{false};
};
} // namespace rs::core::database
