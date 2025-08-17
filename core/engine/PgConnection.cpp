#include "PgConnection.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <openssl/evp.h>

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
  #include <dlfcn.h>
#endif

#include "core/util/Errors.h"
#include "core/transport/SocketTransport.h"
#include "core/transport/TLSTransport.h"

using rs::util::Deadline;
using rs::util::make_deadline;
using rs::core::transport::ITransport;

namespace rs::core::engine {

static inline uint32_t be32(uint32_t v) {
  unsigned char b[4]{
    (unsigned char)((v>>24)&0xFF),
    (unsigned char)((v>>16)&0xFF),
    (unsigned char)((v>>8)&0xFF),
    (unsigned char)(v&0xFF)
  };
  uint32_t r; std::memcpy(&r, b, 4); return r;
}
static inline uint32_t from_be32(const unsigned char* p) {
  return ( (uint32_t)p[0]<<24 ) | ( (uint32_t)p[1]<<16 ) | ( (uint32_t)p[2]<<8 ) | (uint32_t)p[3];
}
static inline uint32_t to_be32(uint32_t v) { return be32(v); }

// Locate directory of the current loaded module (exe or lib) to find root.crt
static std::filesystem::path current_module_dir() {
#if defined(_WIN32)
  HMODULE hm = nullptr;
  if (GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&current_module_dir), &hm))
  {
    char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(hm, buf, MAX_PATH);
    if (n && n < MAX_PATH) return std::filesystem::path(buf).parent_path();
  }
  return {};
#elif defined(__APPLE__) || defined(__linux__)
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&current_module_dir), &info) && info.dli_fname) {
    return std::filesystem::path(info.dli_fname).parent_path();
  }
  return {};
#else
  return {};
#endif
}

PgConnection::PgConnection() = default;
PgConnection::PgConnection(std::unique_ptr<ITransport> t) : tr_(std::move(t)) {}
PgConnection::~PgConnection() = default;

void PgConnection::connect(const Settings& s) {
  settings_ = s;
  auto dl = rs::util::make_deadline(settings_.timeout);

  tr_.reset(); // ensure clean

  if (settings_.sslmode == SslMode::Disable) {
    // ---- Plain TCP only ----
    auto sock = std::make_unique<transport::SocketTransport>();
    sock->connect(settings_.host, settings_.port, dl);
    tr_ = std::move(sock);
  } else {
    // ---- PG SSLRequest then upgrade that same socket to TLS ----
    transport::SocketTransport sock;
    sock.connect(settings_.host, settings_.port, dl);

    // Send SSLRequest: int32 len=8, int32 code=80877103 (0x04D2162F)
    struct { uint32_t len; uint32_t code; } req;
    req.len  = to_be32(8);
    req.code = to_be32(80877103u);
    {
    //   auto r = sock.send(std::as_bytes(std::span{&req, sizeof(req)}), dl);
      auto r = sock.send(std::as_bytes(std::span{&req, 1}), dl);
      if (r.n != sizeof(req)) throw rs::util::IOError("short write for SSLRequest");
    }

    // Read single-byte response: 'S' = OK, 'N' = not supported
    unsigned char resp{};
    {
      auto r = sock.recv(std::span<std::byte>((std::byte*)&resp, 1), dl);
      if (r.n != 1) throw rs::util::IOError("short read for SSLRequest response");
    }
    if (resp != 'S') {
      throw rs::util::IOError("server does not accept SSL (sslmode requires TLS)");
    }

    // Upgrade to TLS on the SAME socket
    auto tls = std::make_unique<transport::TLSTransport>();
    tls->set_min_tls_version(TLS1_2_VERSION);
    tls->set_verify(true); // Verify chain for both VerifyCA and VerifyFull
    tls->set_hostname_verification(settings_.sslmode == SslMode::VerifyFull);

    // CA selection order:
    // 1) explicit file (ssl_ca_file)
    // 2) explicit dir  (ssl_ca_dir)
    // 3) <odbc_library_dir>/root.crt (if present)
    // 4) defaults (inside ensure_ctx)
    std::string ca_file, ca_dir;
    if (!settings_.ssl_ca_file.empty()) {
      ca_file = settings_.ssl_ca_file;
    } else if (!settings_.ssl_ca_dir.empty()) {
      ca_dir = settings_.ssl_ca_dir;
    } else {
      try {
        auto moddir = current_module_dir();
        if (!moddir.empty()) {
          auto candidate = moddir / "root.crt";
          if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
            ca_file = candidate.string();
          }
        }
      } catch (...) { /* ignore; will use defaults */ }
    }
    tls->set_ca_locations(ca_file, ca_dir);

    // Move the native socket from the plain transport into TLS
    auto s_native = sock.release();               // ensures SocketTransport won't close it
    tls->upgrade_from(s_native, settings_.host, dl);

    tr_ = std::move(tls);
  }

  // ---- Protocol startup & auth → ReadyForQuery ----
  send_startup(dl);
  run_until_ready(dl);
}

void PgConnection::send_startup(Deadline dl) {
  std::vector<std::pair<std::string,std::string>> kv;
  kv.push_back({"user", settings_.user});
  kv.push_back({"database", settings_.db});
  if (settings_.extras.find("application_name") == settings_.extras.end())
    kv.push_back({"application_name", "odbc++"});
  for (auto& e : settings_.extras) kv.push_back(e);
  send_startup_frame(kv, dl);
}

// ---- wire I/O ----

void PgConnection::write_all(const void* data, size_t n, Deadline dl) {
  const std::byte* p = reinterpret_cast<const std::byte*>(data);
  size_t off = 0;
  while (off < n) {
    auto r = tr_->send(std::span<const std::byte>(p+off, n-off), dl);
    if (r.n == 0) throw util::IOError("short write");
    off += r.n;
  }
}

void PgConnection::read_exact(void* data, size_t n, Deadline dl) {
  std::byte* p = reinterpret_cast<std::byte*>(data);
  size_t off = 0;
  while (off < n) {
    auto r = tr_->recv(std::span<std::byte>(p+off, n-off), dl);
    if (r.eof) throw util::IOError("eof during read");
    if (r.n == 0) continue;
    off += r.n;
  }
}

// Read a backend message: 1 byte tag + 4-byte length (len includes itself)
std::vector<std::byte> PgConnection::read_message(Deadline dl, char& tag_out) {
  unsigned char hdr[5];
  read_exact(hdr, 5, dl);
  tag_out = (char)hdr[0];
  uint32_t len = from_be32(hdr+1);
  if (len < 4) throw util::IOError("invalid message length");
  uint32_t payload_len = len - 4;
  std::vector<std::byte> payload(payload_len);
  if (payload_len) read_exact(payload.data(), payload_len, dl);
  return payload;
}

// ---- encoders ----

void PgConnection::send_startup_frame(const std::vector<std::pair<std::string,std::string>>& kv, Deadline dl) {
  // length(int32) + protocol(196608) + key\0val\0... \0
  size_t bytes = 4 /*len*/ + 4 /*protocol*/ + 1 /*terminator*/;
  for (auto& p : kv) bytes += p.first.size()+1 + p.second.size()+1;

  std::vector<unsigned char> buf(bytes);

  uint32_t bl = (uint32_t)bytes;
  uint32_t nbl = be32(bl);
  std::memcpy(buf.data(), &nbl, 4);

  uint32_t proto = be32(196608u);
  std::memcpy(buf.data()+4, &proto, 4);

  size_t off = 8;
  auto putz = [&](const std::string& s) {
    std::memcpy(buf.data()+off, s.data(), s.size());
    off += s.size();
    buf[off++] = 0;
  };
  for (auto& p : kv) { putz(p.first); putz(p.second); }
  buf[off++] = 0;

  write_all(buf.data(), buf.size(), dl);
}

void PgConnection::send_simple_password(std::string_view pw, Deadline dl) {
  const char tag = 'p';
  uint32_t len = (uint32_t)(4 + pw.size() + 1);
  uint32_t bl = be32(len);
  write_all(&tag, 1, dl);
  write_all(&bl,  4, dl);
  write_all(pw.data(), pw.size(), dl);
  const char z = 0; write_all(&z, 1, dl);
}

// ---- decoders ----

rs::pg::Authentication PgConnection::decode_auth(const std::vector<std::byte>& payload) {
  if (payload.size() < 4) throw util::IOError("auth payload too short");
  rs::pg::Authentication a{};
  a.raw_code = from_be32(reinterpret_cast<const unsigned char*>(payload.data()));
  if (a.raw_code == 5) { // MD5
    if (payload.size() < 8) throw util::IOError("auth MD5 payload too short");
    std::memcpy(a.md5_salt.data(), payload.data()+4, 4);
  } else if (a.raw_code == 10) {
    // SASL mechanisms list – not used for now
  } else if (a.raw_code == 11 || a.raw_code == 12) {
    a.sasl_data.assign(payload.begin()+4, payload.end());
  }
  return a;
}

rs::pg::ErrorResponse PgConnection::decode_error(const std::vector<std::byte>& payload) {
  rs::pg::ErrorResponse e{};
  const unsigned char* p = reinterpret_cast<const unsigned char*>(payload.data());
  size_t i = 0, n = payload.size();
  while (i < n && p[i] != 0) {
    char code = (char)p[i++];
    size_t start = i;
    while (i < n && p[i] != 0) ++i;
    std::string val((const char*)p+start, (i-start));
    if (i < n && p[i] == 0) ++i;
    e.fields[code] = std::move(val);
  }
  return e;
}

void PgConnection::decode_param_status(const std::vector<std::byte>& payload, std::string& k, std::string& v) {
  const char* p = reinterpret_cast<const char*>(payload.data());
  size_t n = payload.size();
  size_t i = 0;
  size_t s = i; while (i<n && p[i]!=0) ++i; k.assign(p+s, i-s); if (i<n && p[i]==0) ++i;
  s = i; while (i<n && p[i]!=0) ++i; v.assign(p+s, i-s);
}

rs::pg::BackendKeyData PgConnection::decode_bk(const std::vector<std::byte>& payload) {
  if (payload.size() != 8) throw util::IOError("BackendKeyData size != 8");
  rs::pg::BackendKeyData b{};
  b.pid    = from_be32(reinterpret_cast<const unsigned char*>(payload.data()+0));
  b.secret = from_be32(reinterpret_cast<const unsigned char*>(payload.data()+4));
  return b;
}

// ---- auth handling ----

std::string PgConnection::md5_hex(const void* data, size_t n) {
  unsigned char md[EVP_MAX_MD_SIZE]; unsigned int mdlen=0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
  EVP_DigestUpdate(ctx, data, n);
  EVP_DigestFinal_ex(ctx, md, &mdlen);
  EVP_MD_CTX_free(ctx);
  static const char hexd[] = "0123456789abcdef";
  std::string out; out.resize(mdlen*2);
  for (unsigned i=0;i<mdlen;i++){ out[2*i]=hexd[(md[i]>>4)&0xF]; out[2*i+1]=hexd[md[i]&0xF]; }
  return out;
}

void PgConnection::handle_auth(const rs::pg::Authentication& a, Deadline dl) {
  auto k = a.known();
  if (!k) throw util::IOError("unsupported auth method code=" + std::to_string(a.raw_code));

  switch (*k) {
    case rs::pg::Authentication::Known::Ok:
      return;

    case rs::pg::Authentication::Known::Cleartext:
      send_password(settings_.password, dl);
      return;

    case rs::pg::Authentication::Known::MD5: {
      // md5(md5(password+user) + salt)
      std::string step1 = md5_hex((settings_.password + settings_.user).data(),
                                  settings_.password.size()+settings_.user.size());
      std::string salted = step1;
      salted.append(reinterpret_cast<const char*>(a.md5_salt.data()), 4);
      std::string step2 = md5_hex(salted.data(), salted.size());
      std::string final = "md5" + step2;
      send_password(final, dl);
      return;
    }

    case rs::pg::Authentication::Known::SASL:
    case rs::pg::Authentication::Known::SASLContinue:
    case rs::pg::Authentication::Known::SASLFinal:
      throw util::IOError("server requested SASL/SCRAM auth which is not enabled in this build");
  }
}

void PgConnection::send_password(std::string_view pw, Deadline dl) {
  send_simple_password(pw, dl);
}

void PgConnection::run_until_ready(Deadline dl) {
  for (;;) {
    char tag=0;
    auto payload = read_message(dl, tag);
    switch (tag) {
      case 'R': {
        auto a = decode_auth(payload);
        handle_auth(a, dl);
        break;
      }
      case 'S': {
        std::string k,v; decode_param_status(payload, k, v);
        params_[k] = v;
        break;
      }
      case 'K': {
        bk_ = decode_bk(payload);
        break;
      }
      case 'E': {
        last_error_ = decode_error(payload);
        throw util::IOError("server error: " + last_error_.message() + " (SQLSTATE " + last_error_.code() + ")");
      }
      case 'N': {
        // notice: ignore for now
        break;
      }
      case 'Z': {
        if (payload.size()!=1) throw util::IOError("invalid ReadyForQuery");
        tx_status_ = (rs::pg::TxStatus)payload[0];
        return;
      }
      default:
        // ignore
        break;
    }
  }
}

std::string PgConnection::parameterStatus(std::string_view k) const {
  auto it = params_.find(std::string(k));
  return (it==params_.end()) ? std::string{} : it->second;
}

} // namespace rs::core::engine
