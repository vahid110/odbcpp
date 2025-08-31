/*
Example usage:
./build/extended_query_example xxx.redshift.amazonaws.com 5439 dev awsuser ppp 15000 sslmode=verify-full ./root.crt
*/
#include "core/engine/pg_connection.h"
#include "core/util/deadline.h"
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <chrono>

using rs::core::engine::PgConnection;
using rs::core::engine::SslMode;

static const char *tx_to_string(rs::pg::TxStatus s)
{
    switch (s)
    {
    case rs::pg::TxStatus::Idle:
        return "Idle";
    case rs::pg::TxStatus::InTx:
        return "InTransaction";
    case rs::pg::TxStatus::InFailedTx:
        return "Error";
    default:
        return "Unknown";
    }
}

static void print_rows(const std::vector<std::vector<std::string>>& rows) {
  std::cout << "Rows: " << rows.size() << "\n";
  for (const auto& row : rows) {
    for (size_t i = 0; i < row.size(); ++i) {
      if (i) std::cout << " | ";
      std::cout << (row[i].empty() ? "NULL" : row[i]);
    }
    std::cout << "\n";
  }
}

int main(int argc, char** argv) {
  if (argc < 6) {
    std::cerr << "Usage: " << argv[0]
              << " <host> <port> <db> <user> <password> "
                 "[timeout_ms=15000] [sslmode=verify-full] [cafile] [capath]\n";
    return 1;
  }

  const std::string host = argv[1];
  const uint16_t    port = static_cast<uint16_t>(std::stoi(argv[2]));
  const std::string db   = argv[3];
  const std::string user = argv[4];
  const std::string pw   = argv[5];
  const int timeout_ms   = (argc >= 7) ? std::stoi(argv[6]) : 15000;
  const std::string sslm = (argc >= 8) ? argv[7] : "verify-full";
  const std::string caf  = (argc >= 9) ? argv[8] : "";
  const std::string cap  = (argc >= 10) ? argv[9] : "";

  try {
    PgConnection::Settings s;
    s.host = host;
    s.port = port;
    s.db = db;
    s.user = user;
    s.password = pw;
    s.timeout = std::chrono::milliseconds(timeout_ms);
    if (sslm == "disable")       s.sslmode = SslMode::Disable;
    else if (sslm == "verify-ca")  s.sslmode = SslMode::VerifyCA;
    else                            s.sslmode = SslMode::VerifyFull; // default
    if (!caf.empty()) s.ssl_ca_file = caf;
    if (!cap.empty()) s.ssl_ca_dir  = cap;

    PgConnection conn;                 // Let PgConnection handle TCP → SSLRequest → TLS
    conn.connect(s);

    std::cout << "Connected. TxStatus=" << conn.txStatus() << "\n";
    std::cout << "server_version=" << conn.parameterStatus("server_version") << "\n";
    std::cout << "client_encoding=" << conn.parameterStatus("client_encoding") << "\n";

    auto dl = rs::util::make_deadline(std::chrono::milliseconds(timeout_ms));

    // 1) Parameter binding (extended query path)
    {
      std::cout << "\n-- Param exec: SELECT $1::int4 + $2::int4 --\n";
      std::array<std::string,2> params = {"7","35"};
      auto rows = conn.exec_params("SELECT $1::int4 + $2::int4", params, dl /*deadline*/, 0 /*all rows*/);
      print_rows(rows); // expect a single row "42"
    }

    // 2) Multiple rows (and fetch sizing)
    //    Redshift-safe: UNION ALL chain instead of generate_series
    {
      std::cout << "\n-- Multi-row, fetch-size=2 (portal suspension demonstration) --\n";
      const std::string sql =
        "SELECT n FROM (SELECT 1 AS n UNION ALL SELECT 2 UNION ALL SELECT 3 "
        "UNION ALL SELECT 4 UNION ALL SELECT 5) t ORDER BY n";
      std::vector<std::string> no_params;
      // With max_rows=2 the helper returns only first 2 rows (we didn’t loop Execute)
      auto first_batch = conn.exec_params(sql, no_params, dl, /*max_rows=*/2);
      print_rows(first_batch); // expect 2 rows: 1, 2
      // If you want all, call with max_rows=0:
      auto all_rows = conn.exec_params(sql, no_params, dl, /*max_rows=*/0);
      print_rows(all_rows); // expect 5 rows: 1..5
    }

    // 3) Error handling
    {
      std::cout << "\n-- Intentional error: bad SQL --\n";
      try {
        std::vector<std::string> none;
        (void)conn.exec_params("SELECT nope_from_nowhere", none, dl, 0);
        std::cout << "UNEXPECTED: error did not occur\n";
      } catch (const std::exception& ex) {
        std::cout << "Caught error as expected: " << ex.what() << "\n";
      }
    }

    // 4) Transaction flow
    {
      std::cout << "\n-- Transaction demo --\n";
      conn.simpleQuery("BEGIN", dl);
      std::cout << "TxStatus after BEGIN: " << tx_to_string(conn.txStatus()) << "\n";

      auto r = conn.simpleQuery("SELECT 1", dl);
      print_rows(r);

      conn.simpleQuery("ROLLBACK", dl);
      std::cout << "TxStatus after ROLLBACK: " << tx_to_string(conn.txStatus()) << "\n";
    }

    std::cout << "\nDone.\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 2;
  }
}
