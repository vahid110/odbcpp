/*
Example usage :
./build/pg_handshake_example xxx.redshift.amazonaws.com 5439 dev awsuser ppp 15000 ./root.crt
*/
#include "core/database/database_factory.h"
#include "core/util/deadline.h"
#include <iostream>

using namespace rs::core::database;

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "Usage: " << argv[0]
              << " <host> <port> <db> <user> [password] [timeout_ms] [sslmode]\n";
    return 1;
  }

  std::string host = argv[1];
  uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
  std::string db = argv[3];
  std::string user = argv[4];
  std::string pw = (argc >= 6) ? argv[5] : "";
  int timeout_ms = (argc >= 7) ? std::stoi(argv[6]) : 15000;
  std::string sslmode = (argc >= 8) ? argv[7] : "verify-full";

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

    conn->connect(settings);

    std::cout << "Connected OK.\n";
    std::cout << "server_version=" << conn->get_parameter("server_version") << "\n";
    std::cout << "client_encoding=" << conn->get_parameter("client_encoding") << "\n";
    std::cout << "DateStyle=" << conn->get_parameter("DateStyle") << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 2;
  }
}
