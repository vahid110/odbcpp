#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <span>

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
    bool ssl = true; // not used; sslmode drives behavior
    std::chrono::milliseconds timeout{15000};
    SslMode sslmode = SslMode::VerifyFull;
    std::string ssl_ca_file;   // optional: path to CA bundle (PEM)
    std::string ssl_ca_dir;    // optional: path to CA directory (hash-based)
    std::map<std::string,std::string> extras; // application_name, etc.
  };

  PgConnection();
  explicit PgConnection(std::unique_ptr<rs::core::transport::ITransport> t);
  ~PgConnection() = default;

  // Establish connection (TCP -> SSLRequest -> [TLS] -> Startup -> Auth -> Ready)
  void connect(const Settings& s);

  // Introspection
  std::string parameterStatus(std::string_view k) const;
  rs::pg::TxStatus txStatus() const { return tx_status_; }
  const rs::pg::BackendKeyData& backendKey() const { return bk_; }
  const rs::pg::ErrorResponse& lastError() const { return last_error_; }

  // Simple text query (legacy/bootstrap)
  std::vector<std::vector<std::string>>
  simpleQuery(std::string_view sql, rs::util::Deadline dl);

  // ---- Extended Query (minimal) ----
  // Execute with parameters using unnamed statement/portal.
  // param_text: all parameters encoded as text; result columns are returned as text.
  // max_rows: 0 = all rows; >0 lets you exercise portal suspension.
  std::vector<std::vector<std::string>>
  exec_params(std::string_view sql,
              std::span<const std::string> param_text,
              rs::util::Deadline dl,
              int max_rows = 0);

private:
  // transport + session state
  std::unique_ptr<rs::core::transport::ITransport> tr_;
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

  // encoders (startup/simple)
  void send_startup_frame(const std::vector<std::pair<std::string,std::string>>& kv, rs::util::Deadline dl);
  void send_simple_password(std::string_view pw, rs::util::Deadline dl);

  // decoders
  static rs::pg::Authentication decode_auth(const std::vector<std::byte>& payload);
  static rs::pg::ErrorResponse  decode_error(const std::vector<std::byte>& payload);
  static void decode_param_status(const std::vector<std::byte>& payload, std::string& k, std::string& v);
  static rs::pg::BackendKeyData decode_bk(const std::vector<std::byte>& payload);

  // util (MD5 for pg-md5 auth)
  static std::string md5_hex(const void* data, size_t n);

  // ---- Extended Query helpers (encoders) ----
  void send_Parse(std::string_view statement_name,
                  std::string_view sql,
                  std::span<const uint32_t> param_type_oids,
                  rs::util::Deadline dl);

  void send_Bind(std::string_view portal_name,
                 std::string_view statement_name,
                 std::span<const uint16_t> param_formats,
                 std::span<const std::string> param_values, // raw strings (no trailing NUL), -1 means NULL via length -1
                 std::span<const uint16_t> result_formats,  // size 1 (applies to all) or per-column
                 rs::util::Deadline dl);

  void send_Describe(char what /*'S' stmt or 'P' portal*/,
                     std::string_view name,
                     rs::util::Deadline dl);

  void send_Execute(std::string_view portal_name, int max_rows, rs::util::Deadline dl);
  void send_Sync(rs::util::Deadline dl);
};

} // namespace rs::core::engine
