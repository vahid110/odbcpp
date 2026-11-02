#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rs::core::database {

enum class IdentifierCase { Sensitive, Upper, Lower, Mixed };
enum class NullCollation { High, Low, Start, End };
enum class CorrelationNames { None, Different, Any };
enum class GroupBySupport { None, EqualsSelect, ContainsSelect, Unrelated };
enum class InfoSchemaSupport { None, Partial, Full };

// SQL value categories, independent of adapter identifiers and native OIDs.
// Character covers the adapter's narrow/wide and fixed/varying spellings;
// the backend determines the native representation and documented cast limits.
enum class SqlConversionType {
  SmallInteger, Integer, BigInteger, Real, Double, Numeric, Boolean,
  Character, Binary, Date, Time, Timestamp, Uuid, Count
};
constexpr std::size_t sql_conversion_type_count = static_cast<std::size_t>(SqlConversionType::Count);
static_assert(sql_conversion_type_count <= 32);
constexpr std::uint32_t sql_conversion_bit(SqlConversionType type) noexcept {
  const auto index = static_cast<std::size_t>(type);
  return index < sql_conversion_type_count ? std::uint32_t{1} << index : 0;
}

struct SqlConversionSupport {
  std::array<std::uint32_t, sql_conversion_type_count> targets{};
  constexpr std::uint32_t from(SqlConversionType source) const noexcept {
    const auto index = static_cast<std::size_t>(source);
    return index < targets.size() ? targets[index] : 0;
  }
};

enum class SqlIntervalUnit { Nanosecond, Second, Minute, Hour, Day, Week, Month, Quarter, Year, Count };
constexpr std::uint16_t sql_interval_bit(SqlIntervalUnit unit) noexcept {
  const auto index = static_cast<std::size_t>(unit);
  return index < static_cast<std::size_t>(SqlIntervalUnit::Count)
      ? static_cast<std::uint16_t>(std::uint16_t{1} << index) : 0;
}

// No ODBC identifiers or native protocol IDs. Zero limits mean unknown/no
// declared limit. Strings must remain valid for the provider lifetime.
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
  std::string_view keywords{};
  std::uint16_t max_identifier_length{0};
  std::uint16_t max_columns_in_table{0};
  std::uint16_t max_columns_in_select{0};
  IdentifierCase identifier_case{IdentifierCase::Sensitive};
  IdentifierCase quoted_identifier_case{IdentifierCase::Sensitive};
  NullCollation null_collation{NullCollation::High};
  CorrelationNames correlation_names{CorrelationNames::None};
  GroupBySupport group_by{GroupBySupport::None};
  InfoSchemaSupport info_schema_support{InfoSchemaSupport::None};
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
  bool create_table{false};
  bool create_table_commit_preserve{false};
  bool create_table_commit_delete{false};
  bool create_table_column_collation{false};
  bool drop_table{false};
  bool create_view{false};
  bool create_view_check_option{false};
  bool drop_view{false};
  bool alter_table{false};
  bool alter_table_column_collation{false};
  bool create_schema{false};
  bool drop_schema{false};
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
  SqlConversionSupport conversions{};
  std::uint16_t timestamp_add_intervals{0};
  std::uint16_t timestamp_diff_intervals{0};
};
} // namespace rs::core::database
