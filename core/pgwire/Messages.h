#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <map>

namespace rs::pg {

// Transaction status from ReadyForQuery
enum class TxStatus : char { Idle='I', InTx='T', InFailedTx='E' };
inline const char *to_string(TxStatus s)
{
  switch (s)
  {
  case TxStatus::Idle:
    return "Idle";
  case TxStatus::InTx:
    return "InTx";
  case TxStatus::InFailedTx:
    return "InFailedTx";
      default:
    return "Unknown";
  }
}

inline std::ostream &operator<<(std::ostream &os, TxStatus s)
{
  return os << to_string(s);
}

// Authentication message
struct Authentication {
  uint32_t raw_code = 0;

  enum class Known : uint32_t {
    Ok=0, Cleartext=3, MD5=5, SASL=10, SASLContinue=11, SASLFinal=12
  };

  std::optional<Known> known() const {
    switch (raw_code) {
      case 0:  return Known::Ok;
      case 3:  return Known::Cleartext;
      case 5:  return Known::MD5;
      case 10: return Known::SASL;
      case 11: return Known::SASLContinue;
      case 12: return Known::SASLFinal;
      default: return std::nullopt;
    }
  }

  std::array<std::byte,4> md5_salt{};       // for MD5=5
  std::string             sasl_mechanism;   // for SASL=10 (not used now)
  std::vector<std::byte>  sasl_data;        // for 10/11/12 (not used now)
};

struct BackendKeyData { uint32_t pid=0, secret=0; };

struct ErrorResponse {
  // Map of field code -> text, typical keys: 'S','C','M','D','H'
  std::map<char, std::string> fields;
  std::string message() const {
    auto it = fields.find('M');
    return (it!=fields.end()) ? it->second : std::string{};
  }
  std::string code() const {
    auto it = fields.find('C');
    return (it!=fields.end()) ? it->second : std::string{};
  }
};

} // namespace rs::pg
