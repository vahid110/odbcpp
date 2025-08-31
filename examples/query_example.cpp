/*
Example usage:
./build/query_example xxx.redshift.amazonaws.com 5439 dev awsuser yyy 15000 verify-full  ./root.crt
*/
#include "core/database/database_factory.h"
#include "core/util/deadline.h"
#include "core/util/exception_adapter.h"
#include <iostream>

using namespace rs::core::database;

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "Usage: " << argv[0]
              << " <host> <port> <db> <user> [password] [timeout_ms] [sslmode] [cafile]\n";
    return 1;
  }

  std::string host = argv[1];
  uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
  std::string db = argv[3];
  std::string user = argv[4];
  std::string pw = (argc >= 6) ? argv[5] : "";
  int timeout_ms = (argc >= 7) ? std::stoi(argv[6]) : 15000;
  std::string sslmode = (argc >= 8) ? argv[7] : "verify-full";
  std::string cafile = (argc >= 9) ? argv[8] : "";

  try {
    auto conn = DatabaseFactory::create_connection();

    ConnectionSettings settings;
    settings.host = host;
    settings.port = port;
    settings.database = db;
    settings.user = user;
    settings.password = pw;
    settings.timeout = std::chrono::milliseconds(timeout_ms);
    settings.use_ssl = (sslmode != "disable");
    if (!cafile.empty()) settings.ssl_ca_file = cafile;

    rs::util::unwrap_or_throw(conn->connect(settings));

    auto dl = rs::util::make_deadline(std::chrono::milliseconds(timeout_ms));
    auto result = rs::util::unwrap_or_throw(conn->execute_query("SELECT version(), current_date;", dl));

    for (const auto& row : result.rows) {
      for (size_t i = 0; i < row.size(); ++i) {
        if (i) std::cout << " | ";
        std::cout << row[i];
      }
      std::cout << "\n";
    }
    return 0;

  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 2;
  }
}
