#pragma once
#include "i_database_connection.h"
#include "i_protocol_parser.h"
#include "core/transport/i_transport.h"
#include <memory>

namespace rs::core::database {

class GenericDatabaseConnection : public IDatabaseConnection {
public:
  GenericDatabaseConnection(
    std::unique_ptr<IProtocolParser> parser,
    std::unique_ptr<rs::core::transport::ITransport> transport = nullptr);
  
  void connect(const ConnectionSettings& settings) override;
  void disconnect() override;
  bool is_connected() const override;
  
  QueryResult execute_query(std::string_view sql, rs::util::Deadline deadline) override;
  QueryResult execute_prepared(std::string_view sql, 
                             std::span<const std::string> params,
                             rs::util::Deadline deadline) override;
  
  std::string get_parameter(std::string_view key) const override;
  std::string get_last_error() const override;

private:
  std::unique_ptr<IProtocolParser> parser_;
  std::unique_ptr<rs::core::transport::ITransport> transport_;
  ConnectionSettings settings_;
  std::map<std::string, std::string> server_params_;
  std::string last_error_;
  bool connected_ = false;
  
  void write_all(const std::vector<std::byte>& data, rs::util::Deadline deadline);
  std::vector<std::byte> read_message(rs::util::Deadline deadline);
  void perform_authentication(rs::util::Deadline deadline);
  void write_message_to_transport(rs::core::transport::ITransport& transport, const std::vector<std::byte>& data, rs::util::Deadline deadline);
};

} // namespace rs::core::database