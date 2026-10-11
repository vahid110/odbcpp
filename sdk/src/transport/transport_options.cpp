#include "odbcpp/transport/transport_options.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace rs::core::transport {
namespace {

std::string uppercase(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return result;
}

std::string_view trim_ascii(std::string_view value) {
  const auto blank = [](char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
        ch == '\f' || ch == '\v';
  };
  while (!value.empty() && blank(value.front())) value.remove_prefix(1);
  while (!value.empty() && blank(value.back())) value.remove_suffix(1);
  return value;
}

int parse_keepalive_number(std::string_view name, std::string_view raw, int maximum) {
  const auto value = trim_ascii(raw);
  int parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (value.empty() || std::any_of(value.begin(), value.end(), [](char ch) {
        return ch < '0' || ch > '9';
      }) || error != std::errc{} || end != value.data() + value.size() ||
      parsed < 0 || parsed > maximum) {
    throw std::invalid_argument("Invalid " + std::string(name) + " value");
  }
  return parsed;
}

std::size_t parse_positive_size(std::string_view name, std::string_view value) {
  if (value.empty()) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }

  std::size_t parsed = 0;
  const auto [end, error] = std::from_chars(
      value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      parsed == 0) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  return parsed;
}

void apply_parameters(TransportOptions& options,
                      const std::map<std::string, std::string_view>& parameters) {
  for (const auto& [key, raw_value] : parameters) {
    const auto value = uppercase(raw_value);

    if (key == "KEEPALIVE") {
      const auto selected = uppercase(trim_ascii(raw_value));
      if (selected == "1" || selected == "TRUE") options.tcp_keepalive.enabled = true;
      else if (selected == "0" || selected == "FALSE") options.tcp_keepalive.enabled = false;
      else throw std::invalid_argument("Invalid KeepAlive value");
    } else if (key == "KEEPALIVEIDLE") {
      options.tcp_keepalive.idle_seconds = parse_keepalive_number(
          "KeepAliveIdle", raw_value, std::numeric_limits<int>::max());
    } else if (key == "KEEPALIVEINTERVAL") {
      options.tcp_keepalive.interval_seconds = parse_keepalive_number(
          "KeepAliveInterval", raw_value, std::numeric_limits<int>::max());
    } else if (key == "KEEPALIVECOUNT") {
      options.tcp_keepalive.probe_count = parse_keepalive_number("KeepAliveCount", raw_value, 255);
    } else if (key == "TRANSPORTMODE") {
      if (value == "AUTO") options.mode = TransportMode::Auto;
      else if (value == "ASYNC") options.mode = TransportMode::Async;
      else if (value == "SYNC") options.mode = TransportMode::Sync;
      else throw std::invalid_argument("TransportMode must be Auto, Async, or Sync");
    } else if (key == "ASYNCMAXINFLIGHT") {
      options.async_max_inflight = parse_positive_size("AsyncMaxInflight", raw_value);
    } else if (key == "ASYNCQUEUEDEPTH") {
      options.async_queue_depth = parse_positive_size("AsyncQueueDepth", raw_value);
    } else if (key == "ASYNCENGINE") {
      if (value == "AUTO") options.async_engine = AsyncEngine::Auto;
      else if (value == "IOCP") options.async_engine = AsyncEngine::IOCP;
      else if (value == "EPOLL") options.async_engine = AsyncEngine::Epoll;
      else throw std::invalid_argument("AsyncEngine must be Auto, IOCP, or Epoll");
    } else if (key == "DEADLINEMODEL") {
      if (value == "STRICT") options.deadline_model = DeadlineModel::Strict;
      else if (value == "SOCKETTIMEOUT") {
        options.deadline_model = DeadlineModel::SocketTimeout;
      } else {
        throw std::invalid_argument("DeadlineModel must be Strict or SocketTimeout");
      }
    }
  }
}

} // namespace

TransportOptions TransportOptions::resolve(
    const std::map<std::string, std::string>& driver_parameters,
    const std::map<std::string, std::string>& dsn_parameters,
    const std::map<std::string, std::string>& connection_parameters) {
  TransportOptions options;
  std::map<std::string, std::string_view> effective;
  const auto overlay = [&](const auto& layer) {
    for (const auto& [key, value] : layer) {
      const auto normalized = uppercase(key);
      if (normalized == "TRANSPORTMODE" || normalized == "ASYNCMAXINFLIGHT" ||
          normalized == "ASYNCQUEUEDEPTH" || normalized == "ASYNCENGINE" ||
          normalized == "DEADLINEMODEL" || normalized == "KEEPALIVE" ||
          normalized == "KEEPALIVEIDLE" || normalized == "KEEPALIVEINTERVAL" ||
          normalized == "KEEPALIVECOUNT") {
        effective[normalized] = value;
      }
    }
  };
  overlay(driver_parameters);
  overlay(dsn_parameters);
  overlay(connection_parameters);
  apply_parameters(options, effective);

  if (options.async_max_inflight > options.async_queue_depth) {
    throw std::invalid_argument(
        "AsyncMaxInflight cannot exceed AsyncQueueDepth");
  }
  return options;
}

std::string_view to_string(TransportMode mode) noexcept {
  switch (mode) {
    case TransportMode::Auto: return "Auto";
    case TransportMode::Async: return "Async";
    case TransportMode::Sync: return "Sync";
  }
  return "Auto";
}

std::string_view to_string(AsyncEngine engine) noexcept {
  switch (engine) {
    case AsyncEngine::Auto: return "Auto";
    case AsyncEngine::IOCP: return "IOCP";
    case AsyncEngine::Epoll: return "Epoll";
  }
  return "Auto";
}

std::string_view to_string(DeadlineModel model) noexcept {
  switch (model) {
    case DeadlineModel::Strict: return "Strict";
    case DeadlineModel::SocketTimeout: return "SocketTimeout";
  }
  return "Strict";
}

} // namespace rs::core::transport
