#include "core/transport/TLSTransport.h"
#include "core/transport/SocketTransport.h"
#include "core/util/Deadline.h"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <map>

// For OpenSSL version constants
#include <openssl/ssl.h>

using rs::core::transport::TLSTransport;
using rs::core::transport::SocketTransport;

// Helper to map string versions to OpenSSL constants
static std::optional<long> parse_tls_version(const std::string& version_str) {
  static const std::map<std::string, long> version_map = {
    {"1.0", TLS1_VERSION},
    {"1.1", TLS1_1_VERSION},
    {"1.2", TLS1_2_VERSION},
    {"1.3", TLS1_3_VERSION}
  };
  auto it = version_map.find(version_str);
  if (it != version_map.end()) {
    return it->second;
  }
  return std::nullopt;
}

static void usage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << " [tls|plain] <host> <port> [timeout_ms] [min_tls_version]\n"
            << "  e.g. " << argv0 << " tls example.com 443 10000 1.2\n"
            << "       " << argv0 << " tls example.com 443 10000 1.3\n"
            << "       " << argv0 << " plain example.com 80 5000\n"
            << "  min_tls_version is optional for TLS mode. Supported values: 1.0, 1.1, 1.2, 1.3\n";
}
/*
Example:
./connect_example tls example.com 443 10000 1.2
./connect_example plain example.com 80 5000
*/
int main(int argc, char** argv) {
  try {
    // Defaults
    std::string mode = "tls";
    std::string host = "example.com";
    uint16_t port = 443;
    int timeout_ms = 10000;
    std::string min_tls_version_str; // New variable for version string

    if (argc >= 2) mode = argv[1];
    if (argc >= 3) host = argv[2];
    if (argc >= 4) port = static_cast<uint16_t>(std::stoi(argv[3]));
    if (argc >= 5) timeout_ms = std::stoi(argv[4]);
    if (argc >= 6) min_tls_version_str = argv[5]; // New argument parsing

    if (argc == 2 && (mode == "-h" || mode == "--help")) {
      usage(argv[0]);
      return 0;
    }
    if (mode != "tls" && mode != "plain") {
      usage(argv[0]);
      return 1;
    }

    auto deadline = rs::util::make_deadline(std::chrono::milliseconds(timeout_ms));

    // Minimal HTTP/1.1 GET
    std::string req = "GET / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";

    if (mode == "tls") {
      TLSTransport t;
      // --- New code to handle min TLS version ---
      if (!min_tls_version_str.empty()) {
        auto version = parse_tls_version(min_tls_version_str);
        if (version) {
          t.set_min_tls_version(*version);
          std::cout << "[TLS] Enforcing minimum TLS version: " << min_tls_version_str << "\n";
        } else {
          std::cerr << "Warning: Unknown TLS version '" << min_tls_version_str << "'. Using default (1.2).\n";
        }
      }
      // ------------------------------------------
      t.connect(host, port, deadline);
      std::cout << "[TLS] handshake OK to " << host << ":" << port << "\n";

      t.send(std::as_bytes(std::span{req.data(), req.size()}), deadline);

      std::vector<std::byte> buf(8192);
      for (;;) {
        auto r = t.recv(std::span<std::byte>(buf.data(), buf.size()), deadline);
        if (r.n) {
          std::cout.write(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(r.n));
        }
        if (r.eof) break;
      }
      t.close();
    } else {
      SocketTransport s;
      s.connect(host, port, deadline);
      std::cout << "[PLAIN] TCP connect OK to " << host << ":" << port << "\n";

      s.send(std::as_bytes(std::span{req.data(), req.size()}), deadline);

      std::vector<std::byte> buf(8192);
      for (;;) {
        auto r = s.recv(std::span<std::byte>(buf.data(), buf.size()), deadline);
        if (r.n) {
          std::cout.write(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(r.n));
        }
        if (r.eof) break;
      }
      s.close();
    }

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
