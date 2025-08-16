#pragma once
#include <stdexcept>
#include <string>

namespace rs::util {

class IOError : public std::runtime_error {
public:
  explicit IOError(const std::string& msg) : std::runtime_error(msg) {}
};

class TimeoutError : public std::runtime_error {
public:
  explicit TimeoutError(const std::string& msg) : std::runtime_error(msg) {}
};

class TLSError : public std::runtime_error {
public:
  explicit TLSError(const std::string& msg) : std::runtime_error(msg) {}
};

} // namespace rs::util