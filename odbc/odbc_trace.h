#pragma once

#include "odbc_types.h"
#include "odbcpp/util/driver_logging.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// ODBC API trace: one enter and one exit record per application call of an
// exported ODBC function. The adapter owns ODBC signatures, handles and
// diagnostics; the SDK logger owns sinks, escaping, bounds and redaction.
namespace rs::odbc::trace {

// How one parameter is rendered. The C type alone cannot distinguish SQL text
// from identifiers, secrets or output buffers (and unixODBC SQLWCHAR equals
// SQLUSMALLINT), so every exported signature declares its parameter roles.
enum class ArgRole : std::uint8_t {
  Handle,      // handle or window, rendered as an address
  HandleType,  // SQL_HANDLE_* symbol
  Integer,     // attribute, option, type or size value
  Length,      // input length/indicator with SQL_NTS-style symbols
  Pointer,     // opaque application buffer or attribute value; address only
  Text,        // input identifier/value text; Sensitive
  QueryText,   // input SQL text; requires the query opt-in
  ConnectionText,  // input connection string; secrets masked, Sensitive
  Secret,      // password or its length; never written
  OutPointer,  // SQLHANDLE*/SQLPOINTER* written by the driver
  OutInteger,  // integer written by the driver
  OutLength,   // length/indicator written by the driver
  OutText,     // character buffer written by the driver; Sensitive
  OutQueryText,
  OutConnectionText,
  OutSqlState, // fixed six-character SQLSTATE buffer; Public
};

struct ArgSpec {
  std::string_view name;
  ArgRole role;
  // Input text: index of its length argument. Output text: index of its
  // capacity argument in characters of the buffer's own character type.
  std::int8_t related{-1};
};

struct ApiSpec {
  std::string_view name;
  std::span<const ArgSpec> args;
};

// Declarative signature table covering every exported ODBC function.
std::span<const ApiSpec> api_specs() noexcept;
const ApiSpec* find_api_spec(std::string_view name) noexcept;

// Trace configuration is read only from trusted process sources: the driver's
// odbcinst.ini section or Windows driver registration, then process
// environment overrides. DSN and connection strings cannot choose a path.
struct TraceOptions {
  rs::core::logging::LoggingOptions logging;

  bool enabled() const noexcept {
    return logging.level != rs::core::logging::LogLevel::Off;
  }

  using EnvironmentLookup =
      std::function<std::optional<std::string>(std::string_view)>;
  static TraceOptions resolve(
      const std::map<std::string, std::string>& driver_parameters,
      const EnvironmentLookup& environment);
};

// Type-erased argument captured at the C boundary.
struct ArgValue {
  enum class Kind : std::uint8_t { Signed, Unsigned, Address };
  Kind kind{Kind::Address};
  std::int64_t signed_value{};
  std::uint64_t unsigned_value{};
  const void* address{};
  std::uint8_t pointee_size{};   // integral or character pointee width
  bool pointee_signed{false};
  bool pointee_address{false};   // pointer to pointer/handle
};

template <typename T>
ArgValue capture(T value) noexcept {
  ArgValue result;
  if constexpr (std::is_integral_v<T>) {
    if constexpr (std::is_signed_v<T>) {
      result.kind = ArgValue::Kind::Signed;
      result.signed_value = static_cast<std::int64_t>(value);
    } else {
      result.kind = ArgValue::Kind::Unsigned;
      result.unsigned_value = static_cast<std::uint64_t>(value);
    }
  } else if constexpr (std::is_pointer_v<T>) {
    using Pointee = std::remove_cv_t<std::remove_pointer_t<T>>;
    result.address = reinterpret_cast<const void*>(value);
    if constexpr (std::is_integral_v<Pointee>) {
      result.pointee_size = static_cast<std::uint8_t>(sizeof(Pointee));
      result.pointee_signed = std::is_signed_v<Pointee>;
    } else if constexpr (std::is_pointer_v<Pointee>) {
      result.pointee_address = true;
    }
  }
  return result;
}

// Process-wide trace sink. Resolved lazily on the first traced call so calls
// made before any connection (environment and connection allocation, attributes
// and connect itself) are traced.
class Tracer {
public:
  // Null when tracing is disabled; the disabled fast path is one atomic load.
  static std::shared_ptr<Tracer> active() noexcept;
  // Replaces process configuration. nullopt re-reads trusted sources lazily.
  static void configure(std::optional<TraceOptions> options);

  explicit Tracer(TraceOptions options);

  void enter(const ApiSpec& spec, std::uint64_t call,
             std::span<const ArgValue> args) const noexcept;
  void exit(const ApiSpec& spec, std::uint64_t call,
            std::span<const ArgValue> args, SQLRETURN result,
            std::chrono::steady_clock::duration elapsed,
            SQLHANDLE diagnostic_handle) const noexcept;
  std::uint64_t next_call() noexcept { return next_call_.fetch_add(1) + 1; }

private:
  TraceOptions options_;
  std::shared_ptr<rs::core::logging::DriverLogger> logger_;
  std::atomic<std::uint64_t> next_call_{0};
};

std::string_view return_code_name(SQLRETURN result) noexcept;
// Replaces password-like attribute values; braces and escapes are preserved.
std::string mask_connection_string(std::string_view connection_string);

// Interceptor around one exported ODBC entry point. Only the application's
// outermost call is traced; driver-internal re-entry stays invisible.
template <typename Call, typename... Args>
SQLRETURN traced_call(std::string_view name, SQLHANDLE diagnostic_handle,
                      bool outermost, Call&& call, Args... args) noexcept {
  std::shared_ptr<Tracer> tracer;
  const ApiSpec* spec = nullptr;
  if (outermost) {
    tracer = Tracer::active();
    if (tracer) spec = find_api_spec(name);
  }
  if (!spec || spec->args.size() != sizeof...(Args)) {
    return std::forward<Call>(call)();
  }
  const std::array<ArgValue, sizeof...(Args)> values{capture(args)...};
  const auto id = tracer->next_call();
  tracer->enter(*spec, id, values);
  const auto started = std::chrono::steady_clock::now();
  const SQLRETURN result = std::forward<Call>(call)();
  tracer->exit(*spec, id, values, result,
               std::chrono::steady_clock::now() - started, diagnostic_handle);
  return result;
}

}  // namespace rs::odbc::trace
