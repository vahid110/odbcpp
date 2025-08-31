#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <map>

namespace rs::pg {

enum class TxStatus : char { Idle='I', InTx='T', InFailedTx='E' };

struct Authentication {
  uint32_t raw_code = 0;
  enum class Known : uint32_t {
    Ok=0, Cleartext=3, MD5=5, SASL=10, SASLContinue=11, SASLFinal=12
  };
  
  std::optional<Known> known() const {
    switch (raw_code) {
      case 0: return Known::Ok;
      case 3: return Known::Cleartext;
      case 5: return Known::MD5;
      case 10: return Known::SASL;
      case 11: return Known::SASLContinue;
      case 12: return Known::SASLFinal;
      default: return std::nullopt;
    }
  }
  
  std::array<std::byte,4> md5_salt{};
  std::vector<std::byte> sasl_data;
};

struct BackendKeyData { uint32_t pid=0, secret=0; };

struct ErrorResponse {
  std::map<char, std::string> fields;
  std::string message() const {
    auto it = fields.find('M');
    return (it != fields.end()) ? it->second : std::string{};
  }
  std::string code() const {
    auto it = fields.find('C');
    return (it != fields.end()) ? it->second : std::string{};
  }
};

} // namespace rs::pg