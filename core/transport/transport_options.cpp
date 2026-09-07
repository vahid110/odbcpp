#include "transport_options.h"

#include <algorithm>
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

std::size_t parse_positive_size(std::string_view name, const std::string& value) {
  if (value.empty() || value.front() == '-') {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }

  std::size_t consumed = 0;
  unsigned long long parsed = 0;
  try {
    parsed = std::stoull(value, &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  if (consumed != value.size() || parsed == 0 ||
      parsed > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(std::string(name) + " must be a positive integer");
  }
  return static_cast<std::size_t>(parsed);
}

void apply(TransportOptions& options,
           const std::map<std::string, std::string>& parameters) {
  for (const auto& [raw_key, raw_value] : parameters) {
    const auto key = uppercase(raw_key);
    const auto value = uppercase(raw_value);

    if (key == "TRANSPORTMODE") {
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
  apply(options, driver_parameters);
  apply(options, dsn_parameters);
  apply(options, connection_parameters);

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
