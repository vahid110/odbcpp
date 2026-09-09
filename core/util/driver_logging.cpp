#include "driver_logging.h"

#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#ifndef _WIN32
#include <spdlog/sinks/syslog_sink.h>
#include <syslog.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rs::core::logging {
namespace {

std::string uppercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });
  return result;
}

std::string trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(first, last - first + 1));
}

bool parse_bool(std::string_view name, const std::string& value) {
  const auto normalized = uppercase(trim(value));
  if (normalized == "1" || normalized == "TRUE" || normalized == "YES" ||
      normalized == "ON") {
    return true;
  }
  if (normalized == "0" || normalized == "FALSE" || normalized == "NO" ||
      normalized == "OFF") {
    return false;
  }
  throw std::invalid_argument(std::string(name) +
                              " must be true or false");
}

std::size_t parse_positive_size(std::string_view name,
                                const std::string& value) {
  if (value.empty() || value.front() == '-') {
    throw std::invalid_argument(std::string(name) +
                                " must be a positive integer");
  }
  std::size_t consumed = 0;
  unsigned long long parsed = 0;
  try {
    parsed = std::stoull(value, &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(name) +
                                " must be a positive integer");
  }
  if (consumed != value.size() || parsed == 0 ||
      parsed > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(std::string(name) +
                                " must be a positive integer");
  }
  return static_cast<std::size_t>(parsed);
}

LogLevel parse_level(const std::string& value) {
  const auto normalized = uppercase(trim(value));
  if (normalized == "OFF") return LogLevel::Off;
  if (normalized == "ERROR") return LogLevel::Error;
  if (normalized == "WARN" || normalized == "WARNING") return LogLevel::Warn;
  if (normalized == "INFO") return LogLevel::Info;
  if (normalized == "DEBUG") return LogLevel::Debug;
  if (normalized == "TRACE") return LogLevel::Trace;
  throw std::invalid_argument(
      "LogLevel must be Off, Error, Warn, Info, Debug, or Trace");
}

LogFormat parse_format(const std::string& value) {
  const auto normalized = uppercase(trim(value));
  if (normalized == "TEXT") return LogFormat::Text;
  if (normalized == "JSON") return LogFormat::Json;
  throw std::invalid_argument("LogFormat must be Text or Json");
}

std::vector<LogSink> parse_sinks(const std::string& value) {
  std::vector<LogSink> result;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto comma = value.find(',', start);
    const auto item = uppercase(trim(std::string_view(value).substr(
        start, comma == std::string::npos ? std::string::npos
                                          : comma - start)));
    LogSink sink;
    if (item == "FILE") {
      sink = LogSink::File;
    } else if (item == "STDERR" || item == "CONSOLE") {
      sink = LogSink::Stderr;
    } else if (item == "SYSLOG") {
      sink = LogSink::Syslog;
    } else {
      throw std::invalid_argument(
          "LogSink entries must be File, Stderr, or Syslog");
    }
    if (std::find(result.begin(), result.end(), sink) == result.end()) {
      result.push_back(sink);
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return result;
}

void apply(LoggingOptions& options,
           const std::map<std::string, std::string>& parameters) {
  for (const auto& [raw_key, value] : parameters) {
    const auto key = uppercase(raw_key);
    if (key == "LOGLEVEL") {
      options.level = parse_level(value);
    } else if (key == "LOGFORMAT") {
      options.format = parse_format(value);
    } else if (key == "LOGSINK") {
      options.sinks = parse_sinks(value);
    } else if (key == "LOGFILE") {
      options.file = value;
    } else if (key == "LOGMAXSIZE") {
      options.max_file_size = parse_positive_size("LogMaxSize", value);
    } else if (key == "LOGMAXFILES") {
      options.max_files = parse_positive_size("LogMaxFiles", value);
    } else if (key == "LOGASYNC") {
      options.asynchronous = parse_bool("LogAsync", value);
    } else if (key == "LOGQUERIES") {
      options.log_queries = parse_bool("LogQueries", value);
    }
  }
}

spdlog::level::level_enum spdlog_level(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Error: return spdlog::level::err;
    case LogLevel::Warn: return spdlog::level::warn;
    case LogLevel::Info: return spdlog::level::info;
    case LogLevel::Debug: return spdlog::level::debug;
    case LogLevel::Trace: return spdlog::level::trace;
    case LogLevel::Off: return spdlog::level::off;
  }
  return spdlog::level::off;
}

std::string json_escape(std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() + 8);
  for (const unsigned char character : value) {
    switch (character) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character < 0x20) {
          result += "\\u00";
          result.push_back(hex[character >> 4]);
          result.push_back(hex[character & 0x0f]);
        } else {
          result.push_back(static_cast<char>(character));
        }
    }
  }
  return result;
}

std::string text_escape(std::string_view value) {
  std::string result;
  result.reserve(value.size() + 8);
  for (const char character : value) {
    switch (character) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default: result.push_back(character); break;
    }
  }
  return result;
}

std::shared_ptr<spdlog::details::thread_pool> logging_thread_pool() {
  static auto pool =
      std::make_shared<spdlog::details::thread_pool>(8192, std::size_t{1});
  return pool;
}

std::atomic<std::uint64_t> next_logger_id{1};

std::string backend_key(const LoggingOptions& options) {
  std::ostringstream key;
  key << static_cast<int>(options.format) << ':' << options.asynchronous << ':'
      << options.file << ':' << options.max_file_size << ':'
      << options.max_files;
  for (const auto sink : options.sinks) {
    key << ':' << static_cast<int>(sink);
  }
  return key.str();
}

} // namespace

class DriverLogger::Impl {
public:
  explicit Impl(std::shared_ptr<spdlog::logger> value)
      : logger(std::move(value)) {}

  std::shared_ptr<spdlog::logger> logger;
};

LoggingOptions LoggingOptions::resolve(
    const std::map<std::string, std::string>& driver_parameters,
    const std::map<std::string, std::string>& dsn_parameters,
    const std::map<std::string, std::string>& connection_parameters) {
  LoggingOptions options;
  apply(options, driver_parameters);
  apply(options, dsn_parameters);
  apply(options, connection_parameters);

  if (options.level != LogLevel::Off && options.sinks.empty()) {
    options.sinks.push_back(options.file.empty() ? LogSink::Stderr
                                                 : LogSink::File);
  }
  if (options.level != LogLevel::Off &&
      std::find(options.sinks.begin(), options.sinks.end(), LogSink::File) !=
          options.sinks.end() &&
      options.file.empty()) {
    throw std::invalid_argument("LogFile is required when LogSink includes File");
  }
#ifdef _WIN32
  if (options.level != LogLevel::Off &&
      std::find(options.sinks.begin(), options.sinks.end(), LogSink::Syslog) !=
          options.sinks.end()) {
    throw std::invalid_argument("LogSink=Syslog is not available on Windows");
  }
#endif
  return options;
}

std::shared_ptr<DriverLogger> DriverLogger::create(
    const LoggingOptions& options, std::uint64_t connection_id) {
  if (options.level == LogLevel::Off) {
    return std::shared_ptr<DriverLogger>(
        new DriverLogger(options, connection_id, nullptr));
  }

  static std::mutex registry_mutex;
  static std::map<std::string, std::weak_ptr<Impl>> registry;
  const auto key = backend_key(options);
  std::lock_guard registry_lock(registry_mutex);
  if (const auto existing = registry.find(key); existing != registry.end()) {
    if (auto impl = existing->second.lock()) {
      return std::shared_ptr<DriverLogger>(
          new DriverLogger(options, connection_id, std::move(impl)));
    }
  }

  std::vector<spdlog::sink_ptr> sinks;
  sinks.reserve(options.sinks.size());
  for (const auto sink : options.sinks) {
    switch (sink) {
      case LogSink::File:
        sinks.push_back(
            std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                options.file, options.max_file_size, options.max_files));
        break;
      case LogSink::Stderr:
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_sink_mt>());
        break;
      case LogSink::Syslog:
#ifndef _WIN32
        sinks.push_back(std::make_shared<spdlog::sinks::syslog_sink_mt>(
            "odbcpp", LOG_PID, LOG_USER, true));
#endif
        break;
    }
  }

  const auto logger_name =
      "odbcpp-" + std::to_string(next_logger_id.fetch_add(1));
  std::shared_ptr<spdlog::logger> backend;
  if (options.asynchronous) {
    backend = std::make_shared<spdlog::async_logger>(
        logger_name, sinks.begin(), sinks.end(), logging_thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
  } else {
    backend = std::make_shared<spdlog::logger>(logger_name, sinks.begin(),
                                               sinks.end());
  }
  backend->set_level(spdlog::level::trace);
  backend->flush_on(spdlog::level::warn);
  if (options.format == LogFormat::Json) {
    backend->set_pattern(
        "{\"timestamp\":\"%Y-%m-%dT%H:%M:%S.%e%z\","
        "\"level\":\"%l\",\"process\":%P,\"thread\":%t,"
        "\"record\":%v}");
  } else {
    backend->set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] [%l] %v");
  }
  auto impl = std::make_shared<Impl>(std::move(backend));
  registry[key] = impl;
  return std::shared_ptr<DriverLogger>(
      new DriverLogger(options, connection_id, std::move(impl)));
}

DriverLogger::DriverLogger(LoggingOptions options,
                           std::uint64_t connection_id,
                           std::shared_ptr<Impl> impl)
    : options_(std::move(options)), connection_id_(connection_id),
      impl_(std::move(impl)) {}

DriverLogger::~DriverLogger() { flush(); }

bool DriverLogger::enabled(LogLevel level) const noexcept {
  return impl_ && level != LogLevel::Off &&
      static_cast<int>(level) <= static_cast<int>(options_.level);
}

bool DriverLogger::logs_queries() const noexcept {
  return options_.log_queries && enabled(LogLevel::Debug);
}

void DriverLogger::log(LogLevel level, std::string_view event,
                       std::string_view message,
                       std::initializer_list<LogField> fields) const noexcept {
  if (!enabled(level)) return;
  try {
    std::ostringstream output;
    if (options_.format == LogFormat::Json) {
      output << "{\"event\":\"" << json_escape(event)
             << "\",\"connection_id\":" << connection_id_
             << ",\"message\":\"" << json_escape(message) << '"';
      for (const auto& field : fields) {
        output << ",\"" << json_escape(field.key) << "\":\""
               << json_escape(field.value) << '"';
      }
      output << '}';
    } else {
      output << "event=" << event << " connection_id=" << connection_id_
             << " message=\"" << text_escape(message) << '"';
      for (const auto& field : fields) {
        output << ' ' << field.key << "=\"" << text_escape(field.value)
               << '"';
      }
    }
    impl_->logger->log(spdlog_level(level), "{}", output.str());
  } catch (...) {
    // Logging must never change ODBC behavior.
  }
}

void DriverLogger::flush() const noexcept {
  if (!impl_) return;
  try {
    impl_->logger->flush();
  } catch (...) {
  }
}

std::string_view to_string(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Off: return "Off";
    case LogLevel::Error: return "Error";
    case LogLevel::Warn: return "Warn";
    case LogLevel::Info: return "Info";
    case LogLevel::Debug: return "Debug";
    case LogLevel::Trace: return "Trace";
  }
  return "Off";
}

std::string_view to_string(LogFormat format) noexcept {
  return format == LogFormat::Json ? "Json" : "Text";
}

std::string_view to_string(LogSink sink) noexcept {
  switch (sink) {
    case LogSink::File: return "File";
    case LogSink::Stderr: return "Stderr";
    case LogSink::Syslog: return "Syslog";
  }
  return "Stderr";
}

} // namespace rs::core::logging
