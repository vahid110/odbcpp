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

// Backend-declared SQL operations, independent of any client API bit numbering.
// Scalar fields describe the dialect's escape forms, not similarly named native
// functions. False is conservative: no claim, including for unknown providers.
struct SqlCapabilities {
  // Outer join forms.
  bool join_left{false};
  bool join_right{false};
  bool join_full{false};
  bool join_nested{false};
  bool join_unordered_columns{false};
  bool join_inner_with_outer{false};
  bool join_all_comparisons{false};
  // Aggregate operations.
  bool aggregate_average{false};
  bool aggregate_count{false};
  bool aggregate_maximum{false};
  bool aggregate_minimum{false};
  bool aggregate_sum{false};
  bool aggregate_distinct{false};
  bool aggregate_all{false};
  // Scalar string escapes.
  bool scalar_upper{false};
  bool scalar_lower{false};
  bool scalar_trimmed_length{false};
  bool scalar_locate_two_arguments{false};
  // Scalar numeric escapes.
  bool scalar_natural_log{false};
  bool scalar_log10{false};
  // Scalar datetime escapes.
  bool scalar_current_date{false};
  bool scalar_current_time{false};
  bool scalar_current_timestamp{false};
  bool scalar_year{false};
  bool scalar_month{false};
  bool scalar_day{false};
  bool scalar_weekday{false};
  bool scalar_yearday{false};
  bool scalar_hour{false};
  bool scalar_minute{false};
  bool scalar_second{false};
  // Scalar null handling.
  bool scalar_if_null{false};
  // Datetime literals.
  bool date_literal{false};
  bool time_literal{false};
  bool timestamp_literal{false};
  // Subqueries.
  bool subquery_comparison{false};
  bool subquery_exists{false};
  bool subquery_in{false};
  bool subquery_quantified{false};
  bool subquery_correlated{false};
  // Standard predicates.
  bool predicate_exists{false};
  bool predicate_not_null{false};
  bool predicate_null{false};
  bool predicate_like{false};
  bool predicate_in{false};
  bool predicate_between{false};
  bool predicate_comparison{false};
  bool predicate_overlaps{false};
  bool predicate_quantified{false};
  // Standard value expressions.
  bool value_case{false};
  bool value_cast{false};
  bool value_coalesce{false};
  bool value_nullif{false};
  // Row constructors.
  bool row_value{false};
  bool row_null{false};
  bool row_subquery{false};
  // Standard string functions.
  bool string_lower{false};
  bool string_upper{false};
  bool string_substring{false};
  bool string_trim_both{false};
  bool string_trim_leading{false};
  bool string_trim_trailing{false};
  // Standard numeric value functions.
  bool numeric_character_length{false};
  bool numeric_extract{false};
  bool numeric_octet_length{false};
  bool numeric_position{false};
  bool numeric_bit_length{false};
  // Standard datetime functions.
  bool datetime_current_date{false};
  bool datetime_current_time{false};
  bool datetime_current_timestamp{false};
  // Relational joins.
  bool join_cross{false};
  bool join_inner{false};
  bool join_natural{false};
  // Multiple results from a direct query are narrower than full batch support.
  bool direct_result_sequences{false};
  // Explicit batches must work through direct and prepared execution,
  // including parameters; simple-query sequences alone do not qualify.
  bool batch_select{false};
  bool batch_command{false};
  // Per-statement batch counts.
  bool batch_individual_counts{false};
};

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
  SqlCapabilities sql{};
  SqlConversionSupport conversions{};
  std::uint16_t timestamp_add_intervals{0};
  std::uint16_t timestamp_diff_intervals{0};
};
} // namespace rs::core::database
