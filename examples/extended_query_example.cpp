/*
Example usage:
./build/extended_query_example xxx.redshift.amazonaws.com 5439 dev awsuser ppp 15000 sslmode=verify-full ./root.crt
*/
#include "core/database/database_factory.h"
#include "core/util/deadline.h"
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <chrono>

using namespace rs::core::database;

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
    auto conn = DatabaseFactory::create_connection();
    
    ConnectionSettings settings;
    settings.host = host;
    settings.port = port;
    settings.database = db;
    settings.user = user;
    settings.password = pw;
    settings.timeout = std::chrono::milliseconds(timeout_ms);
    settings.use_ssl = (sslm != "disable");
    if (!caf.empty()) settings.ssl_ca_file = caf;

    conn->connect(settings);

    std::cout << "Connected.\n";
    std::cout << "server_version=" << conn->get_parameter("server_version") << "\n";
    std::cout << "client_encoding=" << conn->get_parameter("client_encoding") << "\n";

    auto dl = rs::util::make_deadline(std::chrono::milliseconds(timeout_ms));

    // 1) Parameter binding (extended query path)
    {
      std::cout << "\n-- Param exec: SELECT $1::int4 + $2::int4 --\n";
      std::array<std::string,2> params = {"7","35"};
      auto result = conn->execute_prepared("SELECT $1::int4 + $2::int4", params, dl);
      print_rows(result.rows); // expect a single row "42"
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
      auto result = conn->execute_query(sql, dl);
      print_rows(result.rows); // expect 5 rows: 1..5
    }

    // 3) Error handling
    {
      std::cout << "\n-- Intentional error: bad SQL --\n";
      try {
        std::vector<std::string> none;
        (void)conn->execute_query("SELECT nope_from_nowhere", dl);
        std::cout << "UNEXPECTED: error did not occur\n";
      } catch (const std::exception& ex) {
        std::cout << "Caught error as expected: " << ex.what() << "\n";
      }
    }

    // 4) Transaction flow
    {
      std::cout << "\n-- Transaction demo --\n";
      conn->execute_query("BEGIN", dl);
      std::cout << "Transaction started\n";

      auto r = conn->execute_query("SELECT 1", dl);
      print_rows(r.rows);

      conn->execute_query("ROLLBACK", dl);
      std::cout << "Transaction rolled back\n";
    }

    std::cout << "\nDone.\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 2;
  }
}
