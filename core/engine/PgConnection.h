#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/transport/ITransport.h"
#include "core/util/Deadline.h"
#include "core/pgwire/Messages.h"

namespace rs::core::engine {

enum class SslMode { Disable, VerifyCA, VerifyFull };

class PgConnection {
public:
  struct Settings {
    std::string host, user, password, db;
    uint16_t port = 5439;
    bool ssl = true; // (not used now; sslmode drives behavior)
    std::chrono::milliseconds timeout{15000};
    SslMode sslmode = SslMode::VerifyFull;
    std::string ssl_ca_file;   // optional: path to CA bundle (PEM)
    std::string ssl_ca_dir;    // optional: path to CA directory (hash-based)
    std::map<std::string,std::string> extras; // application_name, client_protocol_version, etc.
  };

  // NEW: default ctor so examples can do PgConnection conn;
  PgConnection();
  explicit PgConnection(std::unique_ptr<transport::ITransport> t);
  ~PgConnection();

  void connect(const Settings& s);  // startup + auth -> ReadyForQuery

  std::string parameterStatus(std::string_view k) const;
  rs::pg::TxStatus txStatus() const { return tx_status_; }
  const rs::pg::BackendKeyData& backendKey() const { return bk_; }
  const rs::pg::ErrorResponse& lastError() const { return last_error_; }

private:
  std::unique_ptr<transport::ITransport> tr_;
  Settings settings_;
  std::map<std::string,std::string> params_;
  rs::pg::BackendKeyData bk_{};
  rs::pg::TxStatus tx_status_{rs::pg::TxStatus::Idle};
  rs::pg::ErrorResponse last_error_{};

  // helpers
  void send_startup(rs::util::Deadline dl);
  void handle_auth(const rs::pg::Authentication& a, rs::util::Deadline dl);
  void send_password(std::string_view pw, rs::util::Deadline dl);
  void run_until_ready(rs::util::Deadline dl);

  // wire I/O
  void write_all(const void* data, size_t n, rs::util::Deadline dl);
  void read_exact(void* data, size_t n, rs::util::Deadline dl);
  std::vector<std::byte> read_message(rs::util::Deadline dl, char& tag_out);

  // encoders
  void send_startup_frame(const std::vector<std::pair<std::string,std::string>>& kv, rs::util::Deadline dl);
  void send_simple_password(std::string_view pw, rs::util::Deadline dl);

  // decoders
  static rs::pg::Authentication decode_auth(const std::vector<std::byte>& payload);
  static rs::pg::ErrorResponse  decode_error(const std::vector<std::byte>& payload);
  static void decode_param_status(const std::vector<std::byte>& payload, std::string& k, std::string& v);
  static rs::pg::BackendKeyData decode_bk(const std::vector<std::byte>& payload);

  // md5
  static std::string md5_hex(const void* data, size_t n);
};

} // namespace rs::core::engine
