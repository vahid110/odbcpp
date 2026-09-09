#include <gtest/gtest.h>

#include "core/util/driver_logging.h"
#include "odbc/connection_string.h"
#include "odbc/odbc_handles.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using rs::core::logging::DriverLogger;
using rs::core::logging::LogFormat;
using rs::core::logging::LogLevel;
using rs::core::logging::LogSink;
using rs::core::logging::LoggingOptions;

std::filesystem::path temporary_directory(std::string_view name) {
  auto directory = std::filesystem::temp_directory_path() /
      (std::string(name) + "-" + std::to_string(std::rand()));
  std::filesystem::create_directories(directory);
  return directory;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

TEST(DriverLoggingOptionsTest, LoggingIsDisabledByDefault) {
  const auto options = LoggingOptions::resolve({}, {}, {});
  EXPECT_EQ(LogLevel::Off, options.level);
  EXPECT_TRUE(options.sinks.empty());
  EXPECT_TRUE(options.asynchronous);
  EXPECT_FALSE(options.log_queries);
}

TEST(DriverLoggingOptionsTest, AppliesDriverDsnAndConnectionPrecedence) {
  const std::map<std::string, std::string> driver{
      {"LogLevel", "Error"}, {"LogFile", "driver.log"},
      {"LogMaxSize", "1024"}, {"LogMaxFiles", "2"}};
  const std::map<std::string, std::string> dsn{
      {"LogLevel", "Debug"}, {"LogFile", "dsn.log"},
      {"LogSink", "File,Stderr"}, {"LogQueries", "yes"}};
  const std::map<std::string, std::string> connection{
      {"LogFormat", "Json"}, {"LogAsync", "false"},
      {"LogMaxSize", "2048"}};

  const auto options = LoggingOptions::resolve(driver, dsn, connection);
  EXPECT_EQ(LogLevel::Debug, options.level);
  EXPECT_EQ(LogFormat::Json, options.format);
  EXPECT_EQ("dsn.log", options.file);
  EXPECT_EQ(2048u, options.max_file_size);
  EXPECT_EQ(2u, options.max_files);
  EXPECT_FALSE(options.asynchronous);
  EXPECT_TRUE(options.log_queries);
  ASSERT_EQ(2u, options.sinks.size());
  EXPECT_EQ(LogSink::File, options.sinks[0]);
  EXPECT_EQ(LogSink::Stderr, options.sinks[1]);
}

TEST(DriverLoggingOptionsTest, RejectsInvalidConfiguration) {
  EXPECT_THROW(LoggingOptions::resolve(
                   {}, {}, {{"LogLevel", "Verbose"}}),
               std::invalid_argument);
  EXPECT_THROW(LoggingOptions::resolve(
                   {}, {}, {{"LogLevel", "Info"}, {"LogSink", "File"}}),
               std::invalid_argument);
  EXPECT_THROW(LoggingOptions::resolve(
                   {}, {}, {{"LogMaxSize", "0"}}),
               std::invalid_argument);
  EXPECT_THROW(LoggingOptions::resolve(
                   {}, {}, {{"LogAsync", "maybe"}}),
               std::invalid_argument);
}

TEST(DriverLoggingOptionsTest, ReadsDriverWideIodbcConfigurationPath) {
  const auto directory = temporary_directory("odbcpp-driver-config");
  const auto path = directory / "custom-odbcinst.ini";
  {
    std::ofstream output(path);
    output << "[ODBCPP Logging Test]\n"
              "LogLevel=Debug\n"
              "LogFormat=Json\n";
  }

  const char* previous = std::getenv("ODBCINSTINI");
  const bool had_previous = previous != nullptr;
  const std::string saved = previous ? previous : "";
#ifdef _WIN32
  _putenv_s("ODBCINSTINI", path.string().c_str());
#else
  setenv("ODBCINSTINI", path.string().c_str(), 1);
#endif
  const auto parameters =
      rs::odbc::DSNReader::read_driver_config("ODBCPP Logging Test");
#ifdef _WIN32
  _putenv_s("ODBCINSTINI", saved.c_str());
#else
  if (had_previous) {
    setenv("ODBCINSTINI", saved.c_str(), 1);
  } else {
    unsetenv("ODBCINSTINI");
  }
#endif

  EXPECT_EQ("Debug", parameters.at("LOGLEVEL"));
  EXPECT_EQ("Json", parameters.at("LOGFORMAT"));
  std::filesystem::remove_all(directory);
}

TEST(DriverLoggerTest, WritesEscapedStructuredJson) {
  const auto directory = temporary_directory("odbcpp-json-log");
  const auto path = directory / "driver.log";
  LoggingOptions options;
  options.level = LogLevel::Trace;
  options.format = LogFormat::Json;
  options.sinks = {LogSink::File};
  options.file = path.string();
  options.asynchronous = false;

  auto logger = DriverLogger::create(options, 42);
  logger->log(LogLevel::Info, "test_event", "line one\n\"line two\"",
              {{"field", "value\\with-tab\t"}});
  logger->flush();
  logger.reset();

  const auto contents = read_file(path);
  EXPECT_NE(std::string::npos, contents.find("\"event\":\"test_event\""));
  EXPECT_NE(std::string::npos, contents.find("\"connection_id\":42"));
  EXPECT_NE(std::string::npos,
            contents.find("line one\\n\\\"line two\\\""));
  EXPECT_NE(std::string::npos,
            contents.find("value\\\\with-tab\\t"));
  std::filesystem::remove_all(directory);
}

TEST(DriverLoggerTest, ConnectionLoggingNeverWritesPassword) {
  const auto directory = temporary_directory("odbcpp-connection-log");
  const auto path = directory / "driver.log";
  {
    rs::odbc::ODBCConnection connection(nullptr);
    ASSERT_EQ(SQL_SUCCESS, connection.set_attribute(SQL_ATTR_LOGIN_TIMEOUT, 1));
    const auto result = connection.connect(
        "SERVER=127.0.0.1;PORT=1;UID=test-user;PWD=top-secret;SSL=0;"
        "TransportMode=Sync;LogLevel=Debug;LogSink=File;LogFile=" +
            path.string() + ";LogAsync=false;LogQueries=true",
        "", "");
    EXPECT_EQ(SQL_ERROR, result);
    connection.disconnect();
  }

  const auto contents = read_file(path);
  EXPECT_NE(std::string::npos, contents.find("connection_start"));
  EXPECT_NE(std::string::npos, contents.find("connection_failed"));
  EXPECT_EQ(std::string::npos, contents.find("top-secret"));
  std::filesystem::remove_all(directory);
}

TEST(DriverLoggerTest, RotatesFilesAtConfiguredSize) {
  const auto directory = temporary_directory("odbcpp-rotating-log");
  const auto path = directory / "driver.log";
  LoggingOptions options;
  options.level = LogLevel::Info;
  options.sinks = {LogSink::File};
  options.file = path.string();
  options.max_file_size = 256;
  options.max_files = 2;
  options.asynchronous = false;

  auto logger = DriverLogger::create(options, 7);
  for (int index = 0; index < 20; ++index) {
    logger->log(LogLevel::Info, "rotation_test",
                "A deliberately long logging record used to force rotation",
                {{"index", std::to_string(index)}});
  }
  logger->flush();
  logger.reset();

  EXPECT_TRUE(std::filesystem::exists(path));
  EXPECT_TRUE(std::filesystem::exists(directory / "driver.1.log"));
  std::filesystem::remove_all(directory);
}

TEST(DriverLoggerTest, SerializesConcurrentWriters) {
  const auto directory = temporary_directory("odbcpp-concurrent-log");
  const auto path = directory / "driver.log";
  LoggingOptions options;
  options.level = LogLevel::Info;
  options.sinks = {LogSink::File};
  options.file = path.string();
  options.max_file_size = 1024 * 1024;
  options.asynchronous = false;

  auto logger = DriverLogger::create(options, 9);
  std::vector<std::thread> writers;
  for (int writer = 0; writer < 4; ++writer) {
    writers.emplace_back([logger, writer] {
      for (int record = 0; record < 100; ++record) {
        logger->log(LogLevel::Info, "concurrent_record", "record",
                    {{"writer", std::to_string(writer)},
                     {"record", std::to_string(record)}});
      }
    });
  }
  for (auto& writer : writers) writer.join();
  logger->flush();
  logger.reset();

  const auto contents = read_file(path);
  EXPECT_EQ(400, static_cast<int>(
                     std::count(contents.begin(), contents.end(), '\n')));
  std::filesystem::remove_all(directory);
}

TEST(DriverLoggerTest, DeliversAsynchronousRecords) {
  const auto directory = temporary_directory("odbcpp-async-log");
  const auto path = directory / "driver.log";
  LoggingOptions options;
  options.level = LogLevel::Info;
  options.sinks = {LogSink::File};
  options.file = path.string();
  options.asynchronous = true;

  auto logger = DriverLogger::create(options, 11);
  logger->log(LogLevel::Info, "async_record", "queued record");
  logger->flush();

  std::string contents;
  for (int attempt = 0; attempt < 100; ++attempt) {
    contents = read_file(path);
    if (contents.find("async_record") != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_NE(std::string::npos, contents.find("async_record"));
  logger.reset();
  std::filesystem::remove_all(directory);
}

} // namespace
