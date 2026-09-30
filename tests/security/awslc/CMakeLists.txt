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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND BUILD_SHARED_LIBS)
  # Upstream cohabitant SONAMEs and ELF symbol versions isolate system OpenSSL.
  set(ENABLE_DIST_PKG ON CACHE BOOL "" FORCE)
  set(ENABLE_SYMBOL_VERSIONING ON CACHE BOOL "" FORCE)
else()
  set(ENABLE_DIST_PKG OFF CACHE BOOL "" FORCE)
  set(ENABLE_SYMBOL_VERSIONING OFF CACHE BOOL "" FORCE)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_link_options("-Wl,--exclude-libs,ALL")
endif()
set(ODBCPP_AWSLC_COMMIT "574fbd729ca31aeebe80a98d35742c0402435790")
set(ODBCPP_AWSLC_ARCHIVE_SHA256 "c56d5a8517c0b9b5f79c7a850c0867dbfb3790dae17b447cd9174a0ebf2d037a")
FetchContent_Declare(awslc
  URL https://codeload.github.com/aws/aws-lc/tar.gz/${ODBCPP_AWSLC_COMMIT}
  URL_HASH SHA256=${ODBCPP_AWSLC_ARCHIVE_SHA256}
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

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
add_test(NAME package_archive_integrity
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_package_archive.py")
find_program(OPENSSL_EXECUTABLE openssl REQUIRED)
add_executable(tls_probe tls_probe.cpp)
target_link_libraries(tls_probe PRIVATE adapter)
add_test(NAME verified_tls_interop
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_verified_tls.py"
    --probe "$<TARGET_FILE:tls_probe>" --openssl "${OPENSSL_EXECUTABLE}")
set_tests_properties(verified_tls_interop PROPERTIES TIMEOUT 90)

# Compile the real PostgreSQL session path, without enabling the main driver
# profile or introducing a second implementation of its protocol in the probe.
add_library(postgres_session STATIC
  ${ODBCPP_SOURCE}/core/database/generic_database_connection.cpp
  ${ODBCPP_SOURCE}/core/database/postgres/pg_protocol_parser.cpp
  ${ODBCPP_SOURCE}/core/database/postgres/pg_command.cpp
  ${ODBCPP_SOURCE}/core/database/postgres/pg_sql_dialect.cpp
  ${ODBCPP_SOURCE}/core/database/postgres/pg_type_info.cpp
  ${ODBCPP_SOURCE}/core/transport/socket_transport.cpp
  ${ODBCPP_SOURCE}/core/transport/tls_transport.cpp
  ${ODBCPP_SOURCE}/core/transport/async_tls_transport.cpp
  ${ODBCPP_SOURCE}/core/transport/thread_pool_transport.cpp
  ${ODBCPP_SOURCE}/core/transport/epoll_transport.cpp
  ${ODBCPP_SOURCE}/core/util/utf8.cpp)
find_package(Threads REQUIRED)
target_link_libraries(postgres_session PUBLIC adapter Threads::Threads)
add_executable(test_postgres_live test_postgres_live.cpp)
target_link_libraries(test_postgres_live PRIVATE postgres_session GTest::gtest_main)
add_executable(test_async_tls_transport "${ODBCPP_SOURCE}/tests/unit/test_async_tls_transport.cpp")
target_link_libraries(test_async_tls_transport PRIVATE postgres_session ssl crypto GTest::gtest_main)
add_test(NAME test_async_tls_transport COMMAND test_async_tls_transport)
set_tests_properties(test_async_tls_transport PROPERTIES TIMEOUT 90)
option(ODBCPP_AWSLC_LIVE_TESTS "Require the configured live PostgreSQL TLS/SCRAM fixture" OFF)
if(ODBCPP_AWSLC_LIVE_TESTS)
  add_test(NAME postgres_tls_scram COMMAND test_postgres_live)
  set_tests_properties(postgres_tls_scram PROPERTIES TIMEOUT 90)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_library(ODBCPP_HOST_CRYPTO NAMES libcrypto.so.3 REQUIRED)
  find_library(ODBCPP_HOST_SSL NAMES libssl.so.3 REQUIRED)
  file(REAL_PATH "${ODBCPP_HOST_CRYPTO}" _host_crypto_path)
  file(REAL_PATH "${ODBCPP_HOST_SSL}" _host_ssl_path)
  file(SHA256 "${_host_crypto_path}" _host_crypto_hash)
  file(SHA256 "${_host_ssl_path}" _host_ssl_hash)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/preload-host.json"
    "{\"cryptoPath\":\"${_host_crypto_path}\",\"cryptoSha256\":\"${_host_crypto_hash}\",\"sslPath\":\"${_host_ssl_path}\",\"sslSha256\":\"${_host_ssl_hash}\",\"qualificationClaimed\":false}\n")
  add_executable(test_provider_coexistence test_provider_coexistence.cpp)
  target_link_libraries(test_provider_coexistence PRIVATE adapter GTest::gtest_main ${CMAKE_DL_LIBS})
  target_compile_definitions(test_provider_coexistence PRIVATE
    "ODBCPP_HOST_CRYPTO=\"${ODBCPP_HOST_CRYPTO}\""
    "ODBCPP_HOST_SSL=\"${ODBCPP_HOST_SSL}\"")
  add_test(NAME provider_coexistence COMMAND test_provider_coexistence)
  add_test(NAME preloaded_crypto_primitives COMMAND test_crypto_primitives)
  add_test(NAME preloaded_async_tls COMMAND test_async_tls_transport)
  set(_preload_tests provider_coexistence preloaded_crypto_primitives preloaded_async_tls)
  if(ODBCPP_AWSLC_LIVE_TESTS)
    add_test(NAME preloaded_postgres_tls_scram COMMAND test_postgres_live)
    list(APPEND _preload_tests preloaded_postgres_tls_scram)
  endif()
  set_tests_properties(${_preload_tests} PROPERTIES
    ENVIRONMENT "LD_PRELOAD=${ODBCPP_HOST_SSL}:${ODBCPP_HOST_CRYPTO}" TIMEOUT 90)
  add_test(NAME coexistence_requires_preload
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_preload_required.py"
      $<TARGET_FILE:test_provider_coexistence>)
  set_tests_properties(coexistence_requires_preload PROPERTIES TIMEOUT 30)
  find_program(READELF_EXECUTABLE readelf REQUIRED)
  find_program(LDD_EXECUTABLE ldd REQUIRED)
  if(BUILD_SHARED_LIBS)
    set(_proof_linkage BUNDLED_SHARED)
  else()
    set(_proof_linkage BUNDLED_STATIC)
    execute_process(COMMAND "${CMAKE_LINKER}" --version
      OUTPUT_VARIABLE _linker_version COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCH "^[^\n]+" _linker_identity "${_linker_version}")
    if(NOT _linker_identity MATCHES "^GNU ld ")
      message(FATAL_ERROR "Static proof artifact evidence requires GNU ld")
    endif()
    target_link_options(test_postgres_live PRIVATE
      "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/postgres-link.map")
  endif()
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/linkage-manifest.json" CONTENT
    "{\"provider\":\"AWS_LC\",\"requestedLinkage\":\"${_proof_linkage}\",\"dependencyRoot\":\"${awslc_BINARY_DIR}\",\"sslLinkInput\":\"$<TARGET_FILE:ssl>\",\"cryptoLinkInput\":\"$<TARGET_FILE:crypto>\"}\n")
  add_test(NAME postgres_artifact_linkage
    COMMAND "${CMAKE_COMMAND}"
      "-DARTIFACT=$<TARGET_FILE:test_postgres_live>"
      -DEXPECTED_PROVIDER=AWS_LC "-DEXPECTED_LINKAGE=${_proof_linkage}"
      -DPLATFORM=Linux "-DINSPECTOR=${READELF_EXECUTABLE}" "-DRESOLVER=${LDD_EXECUTABLE}"
      "-DCONFIG_MANIFEST=${CMAKE_CURRENT_BINARY_DIR}/linkage-manifest.json"
      "-DSTATIC_LINK_MAP=${CMAKE_CURRENT_BINARY_DIR}/postgres-link.map"
      "-DSTATIC_LINKER_IDENTITY=${_linker_identity}"
      "-DEVIDENCE=${CMAKE_CURRENT_BINARY_DIR}/postgres-artifact-evidence.json"
      -P "${ODBCPP_SOURCE}/cmake/InspectCryptoArtifact.cmake")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include("${CMAKE_CURRENT_SOURCE_DIR}/DriverProof.cmake")
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/adapter-proof.json"
  "{\"sourceCommit\":\"574fbd729ca31aeebe80a98d35742c0402435790\","
  "\"sourceArchiveSha256\":\"c56d5a8517c0b9b5f79c7a850c0867dbfb3790dae17b447cd9174a0ebf2d037a\","
  "\"requestedSharedBuild\":\"${BUILD_SHARED_LIBS}\","
  "\"requestedPostgresLiveTests\":\"${ODBCPP_AWSLC_LIVE_TESTS}\","
  "\"distributionPackagingRequested\":\"${ENABLE_DIST_PKG}\","
  "\"symbolVersioningRequested\":\"${ENABLE_SYMBOL_VERSIONING}\","
  "\"scope\":\"isolated-driver-and-session-tests\",\"qualificationClaimed\":false}\n")
