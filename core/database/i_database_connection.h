#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <memory>
#include <chrono>
#include "core/util/deadline.h"
#include "core/util/result.h"
#include "query_parameter.h"
#include "query_result.h"

namespace rs::core::database {

struct ConnectionSettings {
  std::string host;
  std::string user;
  std::string password;
  std::string database;
  uint16_t port = 5432;
  std::chrono::milliseconds timeout{15000};
  bool use_ssl = true;
  std::string ssl_ca_file;
  std::string ssl_ca_dir;
};

class IDatabaseConnection {
public:
  virtual ~IDatabaseConnection() = default;
  
  virtual rs::util::Result<void> connect(const ConnectionSettings& settings) = 0;
  virtual void disconnect() = 0;
  virtual bool is_connected() const = 0;
  
  virtual rs::util::Result<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline) = 0;
  virtual rs::util::Result<QueryResult> execute_prepared(std::string_view sql, 
                                                       std::span<const QueryParameter> params,
                                                       rs::util::Deadline deadline) = 0;
  virtual rs::util::Result<QueryResult> describe_statement(
      std::string_view sql,
      std::span<const QueryParameterType> parameter_types,
      rs::util::Deadline deadline) = 0;

  rs::util::Result<QueryResult> execute_prepared(
      std::string_view sql, std::span<const std::string> params,
      rs::util::Deadline deadline) {
    std::vector<QueryParameter> converted;
    converted.reserve(params.size());
    for (const auto& value : params) {
      converted.push_back(QueryParameter{value, QueryParameterType::Unspecified});
    }
    return execute_prepared(sql, converted, deadline);
  }
  
  virtual std::string get_parameter(std::string_view key) const = 0;
  virtual std::string get_last_error() const = 0;
};

} // namespace rs::core::database
