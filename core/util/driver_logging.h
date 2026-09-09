#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rs::core::logging {

enum class LogLevel { Off, Error, Warn, Info, Debug, Trace };
enum class LogFormat { Text, Json };
enum class LogSink { File, Stderr, Syslog };

struct LoggingOptions {
  LogLevel level{LogLevel::Off};
  LogFormat format{LogFormat::Text};
  std::vector<LogSink> sinks;
  std::string file;
  std::size_t max_file_size{10 * 1024 * 1024};
  std::size_t max_files{5};
  bool asynchronous{true};
  bool log_queries{false};

  static LoggingOptions resolve(
      const std::map<std::string, std::string>& driver_parameters,
      const std::map<std::string, std::string>& dsn_parameters,
      const std::map<std::string, std::string>& connection_parameters);
};

struct LogField {
  std::string_view key;
  std::string value;
};

class DriverLogger {
public:
  static std::shared_ptr<DriverLogger> create(
      const LoggingOptions& options, std::uint64_t connection_id);
  ~DriverLogger();

  DriverLogger(const DriverLogger&) = delete;
  DriverLogger& operator=(const DriverLogger&) = delete;

  bool enabled(LogLevel level) const noexcept;
  bool logs_queries() const noexcept;
  void log(LogLevel level, std::string_view event, std::string_view message,
           std::initializer_list<LogField> fields = {}) const noexcept;
  void flush() const noexcept;

private:
  class Impl;
  DriverLogger(LoggingOptions options, std::uint64_t connection_id,
               std::shared_ptr<Impl> impl);

  LoggingOptions options_;
  std::uint64_t connection_id_{};
  std::shared_ptr<Impl> impl_;
};

std::string_view to_string(LogLevel level) noexcept;
std::string_view to_string(LogFormat format) noexcept;
std::string_view to_string(LogSink sink) noexcept;

} // namespace rs::core::logging
