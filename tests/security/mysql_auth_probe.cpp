#include "core/database/mysql/authentication.h"
#include "core/transport/tls_transport.h"
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
  using namespace rs::core::database::mysql;
  using rs::util::DbErrorCode;
  if (argc != 5) return 2; // host, port, CA, expected result
  const char* username=std::getenv("ODBCPP_MYSQL_TEST_USER");
  const char* password=std::getenv("ODBCPP_MYSQL_TEST_PASSWORD");
  if (!username || !password) return 2;
  try {
    const auto port=std::stoul(argv[2]);
    if (port==0 || port>65535) return 2;
    rs::core::transport::TLSTransport transport;
    transport.set_ca_locations(argv[3],"");
    const std::string_view expected=argv[4];
    auto result=authenticate_verified_tls(transport,argv[1],static_cast<std::uint16_t>(port),
        username,password,rs::util::make_deadline(std::chrono::seconds(15)));
    if (expected=="reject-auth" || expected=="reject-tls") {
      if (result) { transport.close(); return 1; }
      const auto error=expected=="reject-auth"?DbErrorCode::AuthenticationFailed:DbErrorCode::TLSError;
      if (result.error()!=error) return 1;
      std::cout << "PASS " << expected << '\n'; return 0;
    }
    if (!result) { std::cerr << "Authentication rejected, code " << result.error().value() << '\n'; return 1; }
    const auto path=expected=="full"?AuthenticationPath::FullOverTls:AuthenticationPath::Cached;
    if (result->path!=path || result->verified.greeting.server_version!="8.4.11" ||
        !transport.peer_identity_verified()) { transport.close(); return 1; }
    transport.close();
    std::cout << "PASS " << expected << '\n';
    return 0;
  } catch (...) { std::cerr << "MySQL probe failed\n"; return 1; }
}
