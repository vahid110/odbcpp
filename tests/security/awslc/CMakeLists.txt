cmake_minimum_required(VERSION 3.20)
project(odbcpp_awslc_adapter_proof LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
  message(FATAL_ERROR "The AWS-LC adapter proof runs only on Linux or local macOS")
endif()
# An isolated adapter experiment: the main driver profile stays disabled.
include(FetchContent)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_TOOL OFF CACHE BOOL "" FORCE)
set(FIPS OFF CACHE BOOL "" FORCE)
set(ENABLE_SOURCE_MODIFICATION OFF CACHE BOOL "" FORCE)
FetchContent_Declare(awslc
  URL https://codeload.github.com/aws/aws-lc/tar.gz/574fbd729ca31aeebe80a98d35742c0402435790
  URL_HASH SHA256=c56d5a8517c0b9b5f79c7a850c0867dbfb3790dae17b447cd9174a0ebf2d037a
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(awslc)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
get_filename_component(ODBCPP_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_library(adapter STATIC
  ${ODBCPP_SOURCE}/core/security/openssl_crypto.cpp
  ${ODBCPP_SOURCE}/core/security/openssl_tls_client.cpp
  ${ODBCPP_SOURCE}/core/database/postgres/scram_sha256.cpp
  ${ODBCPP_SOURCE}/core/util/base64.cpp)
target_include_directories(adapter PUBLIC "${ODBCPP_SOURCE}")
target_link_libraries(adapter PRIVATE ssl crypto)
enable_testing()
foreach(test IN ITEMS test_crypto_primitives test_scram_sha256 test_tls_client)
  add_executable(${test} "${ODBCPP_SOURCE}/tests/unit/${test}.cpp")
  target_compile_definitions(${test} PRIVATE ODBCPP_TEST_AWS_LC=1)
  target_link_libraries(${test} PRIVATE adapter GTest::gtest_main)
  add_test(NAME ${test} COMMAND ${test})
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(OPENSSL_EXECUTABLE openssl REQUIRED)
add_executable(tls_probe tls_probe.cpp)
target_link_libraries(tls_probe PRIVATE adapter)
add_test(NAME verified_tls_interop
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_verified_tls.py"
    --probe "$<TARGET_FILE:tls_probe>" --openssl "${OPENSSL_EXECUTABLE}")
set_tests_properties(verified_tls_interop PROPERTIES TIMEOUT 90)

# Compile the real PostgreSQL session path, without enabling the main driver
# profile or introducing a second implementation of its protocol in the probe.
add_executable(test_postgres_live test_postgres_live.cpp
  ${ODBCPP_SOURCE}/core/database/generic_database_connection.cpp
  ${ODBCPP_SOURCE}/core/database/postgres/pg_protocol_parser.cpp
  ${ODBCPP_SOURCE}/core/database/postgres/pg_command.cpp
  ${ODBCPP_SOURCE}/core/database/postgres/pg_sql_dialect.cpp
  ${ODBCPP_SOURCE}/core/database/postgres/pg_type_info.cpp
  ${ODBCPP_SOURCE}/core/transport/socket_transport.cpp
  ${ODBCPP_SOURCE}/core/transport/tls_transport.cpp
  ${ODBCPP_SOURCE}/core/util/utf8.cpp)
target_link_libraries(test_postgres_live PRIVATE adapter GTest::gtest_main)
option(ODBCPP_AWSLC_LIVE_TESTS "Require the configured live PostgreSQL TLS/SCRAM fixture" OFF)
if(ODBCPP_AWSLC_LIVE_TESTS)
  add_test(NAME postgres_tls_scram COMMAND test_postgres_live)
  set_tests_properties(postgres_tls_scram PROPERTIES TIMEOUT 90)
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/adapter-proof.json"
  "{\"sourceCommit\":\"574fbd729ca31aeebe80a98d35742c0402435790\","
  "\"sourceArchiveSha256\":\"c56d5a8517c0b9b5f79c7a850c0867dbfb3790dae17b447cd9174a0ebf2d037a\","
  "\"requestedSharedBuild\":\"${BUILD_SHARED_LIBS}\","
  "\"requestedPostgresLiveTests\":\"${ODBCPP_AWSLC_LIVE_TESTS}\","
  "\"scope\":\"adapter-and-postgres-session-tests\",\"qualificationClaimed\":false}\n")
