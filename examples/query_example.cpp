/*
Example usage:
./build/query_example xxx.redshift.amazonaws.com 5439 dev awsuser yyy 15000 verify-full  ./root.crt
*/
#include "core/engine/pg_connection.h"
#include "core/util/deadline.h"
#include <iostream>

using rs::core::engine::PgConnection;
using rs::core::engine::SslMode;

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "Usage: " << argv[0]
              << " <host> <port> <db> <user> [password] [timeout_ms] [sslmode] [cafile] [capath]\n";
    return 1;
  }

  std::string host = argv[1];
  uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
  std::string db = argv[3];
  std::string user = argv[4];
  std::string pw = (argc >= 6) ? argv[5] : "";
  int timeout_ms = (argc >= 7) ? std::stoi(argv[6]) : 15000;
  std::string sslmode = (argc >= 8) ? argv[7] : "verify-full";
  std::string cafile  = (argc >= 9) ? argv[8] : "";
  std::string capath  = (argc >= 10) ? argv[9] : "";

  try {
    PgConnection conn; // ✅ let PgConnection handle TCP → SSLRequest → TLS

    PgConnection::Settings s;
    s.host = host;
    s.port = port;
    s.db = db;
    s.user = user;
    s.password = pw;
    s.timeout = std::chrono::milliseconds(timeout_ms);

    if (sslmode == "disable")      s.sslmode = SslMode::Disable;
    else if (sslmode == "verify-ca")   s.sslmode = SslMode::VerifyCA;
    else                               s.sslmode = SslMode::VerifyFull; // default

    if (!cafile.empty()) s.ssl_ca_file = cafile;
    if (!capath.empty()) s.ssl_ca_dir  = capath;

    conn.connect(s);

    auto dl = rs::util::make_deadline(std::chrono::milliseconds(timeout_ms));
    auto rows = conn.simpleQuery("SELECT version(), current_date;", dl);

    for (const auto& row : rows) {
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
