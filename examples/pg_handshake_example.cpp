/*
Example usage :
./build/pg_handshake_example xxx.redshift.amazonaws.com 5439 dev awsuser ppp 15000 ./root.crt
*/
#include "core/engine/pg_connection.h"
#include "core/util/deadline.h"
#include <iostream>

using rs::core::engine::PgConnection;
using rs::core::engine::SslMode;

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
    PgConnection conn;
    PgConnection::Settings s;
    s.host = host;
    s.port = port;
    s.db = db;
    s.user = user;
    s.password = pw;
    s.timeout = std::chrono::milliseconds(timeout_ms);

    if (sslmode == "disable") {
      s.sslmode = SslMode::Disable;
    } else if (sslmode == "verify-ca") {
      s.sslmode = SslMode::VerifyCA;
    } else { // default to verify-full
      s.sslmode = SslMode::VerifyFull;
    }

    // Optional: set these from env/flags if you want to test explicit CA locations
    // s.ssl_ca_file = "path/to/root.crt";
    // s.ssl_ca_dir  = "/etc/ssl/certs";

    s.extras["application_name"] = "odbc++-handshake";

    conn.connect(s);

    std::cout << "Connected OK. tx=" << (char)conn.txStatus() << "\n";
    std::cout << "server_version=" << conn.parameterStatus("server_version") << "\n";
    std::cout << "client_encoding=" << conn.parameterStatus("client_encoding") << "\n";
    std::cout << "DateStyle=" << conn.parameterStatus("DateStyle") << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 2;
  }
}
