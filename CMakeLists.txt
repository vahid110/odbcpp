cmake_minimum_required(VERSION 3.20)
project(odbcpp VERSION 1.0.0 LANGUAGES CXX)

# ---- Project Options ----
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TESTING "Build tests" ON)
include(cmake/CryptoProfile.cmake)
set(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE "" CACHE STRING
    "Expected driver-side SQLWCHAR size (2 or 4; empty disables the ABI check)")
set_property(CACHE ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE PROPERTY STRINGS "" "2" "4")
set(ODBC_DRIVER_MANAGER_FLAVOR "AUTO" CACHE STRING
    "ODBC driver manager used by the external integration test")
set_property(CACHE ODBC_DRIVER_MANAGER_FLAVOR PROPERTY STRINGS
             "AUTO" "UNIXODBC" "IODBC")
set(ODBC_DRIVER_MANAGER_INCLUDE_DIR "" CACHE PATH
    "ODBC application headers used by the driver-manager integration test")
set(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE "" CACHE STRING
    "Expected application-side SQLWCHAR size in the driver-manager test")
set_property(CACHE ODBCPP_EXPECT_DM_SQLWCHAR_SIZE PROPERTY STRINGS "" "2" "4")

if(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE AND
   NOT ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE MATCHES "^[24]$")
  message(FATAL_ERROR "ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE must be 2 or 4")
endif()
if(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE AND
   NOT ODBCPP_EXPECT_DM_SQLWCHAR_SIZE MATCHES "^[24]$")
  message(FATAL_ERROR "ODBCPP_EXPECT_DM_SQLWCHAR_SIZE must be 2 or 4")
endif()

# ---- Build Configuration Info ----
message(STATUS "Build Configuration:")
message(STATUS "  Crypto provider: ${ODBCPP_CRYPTO_PROVIDER}")
message(STATUS "  Crypto linkage: ${ODBCPP_CRYPTO_LINKAGE}")
message(STATUS "  Examples: ${BUILD_EXAMPLES}")
message(STATUS "  Testing: ${BUILD_TESTING}")

# ---- Database Selection (Single Database Per Build) ----
set(TARGET_DATABASE "REDSHIFT" CACHE STRING "Target database for this build")
set_property(CACHE TARGET_DATABASE PROPERTY STRINGS "POSTGRESQL" "REDSHIFT")

if(NOT TARGET_DATABASE STREQUAL "POSTGRESQL" AND
   NOT TARGET_DATABASE STREQUAL "REDSHIFT")
  message(FATAL_ERROR
    "TARGET_DATABASE=${TARGET_DATABASE} is not implemented. "
    "Supported build products are POSTGRESQL and REDSHIFT.")
endif()

message(STATUS "Building ODBC driver for: ${TARGET_DATABASE}")

# ---- Global Settings ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Platform-specific Configuration ----
if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures" FORCE)
endif()

# ---- Dependencies ----
include(cmake/FindDependencies.cmake)
include(cmake/GenerateCryptoManifest.cmake)

# Find ODBC headers
if(APPLE)
  find_path(ODBC_INCLUDE_DIR sql.h PATHS /opt/homebrew/include)
  if(ODBC_INCLUDE_DIR)
    message(STATUS "ODBC headers found: ${ODBC_INCLUDE_DIR}")
  else()
    message(FATAL_ERROR "ODBC headers not found. Install with: brew install unixodbc")
  endif()
elseif(UNIX)
  find_path(ODBC_INCLUDE_DIR sql.h PATHS /usr/include)
  if(NOT ODBC_INCLUDE_DIR)
    message(FATAL_ERROR "ODBC headers not found. Install with: apt-get install unixodbc-dev")
  endif()
endif()

# ---- Core Library ----
# Base sources (always included)
file(GLOB CORE_BASE_SOURCES
  "core/database/*.cpp"
  "core/database/*.h"
  "core/transport/*.cpp"
  "core/transport/*.h"
  "core/security/*.cpp"
  "core/security/*.h"
  "core/util/*.h"
  "core/util/*.cpp"
)

set(CORE_SOURCES ${CORE_BASE_SOURCES})
list(APPEND CORE_SOURCES "product/compiled_backend.cpp")

# Add database-specific sources based on TARGET_DATABASE
if(TARGET_DATABASE STREQUAL "POSTGRESQL" OR TARGET_DATABASE STREQUAL "REDSHIFT")
  # Core database protocol
  file(GLOB DB_SOURCES "core/database/postgres/*.cpp" "core/database/postgres/*.h")
  list(APPEND CORE_SOURCES ${DB_SOURCES})
  
  # Database-specific ODBC layer
  if(TARGET_DATABASE STREQUAL "REDSHIFT")
    file(GLOB ODBC_DB_SOURCES "odbc/redshift/*.cpp" "odbc/redshift/*.h")
    list(APPEND CORE_SOURCES ${ODBC_DB_SOURCES})
  else()
    file(GLOB ODBC_DB_SOURCES "odbc/postgresql/*.cpp" "odbc/postgresql/*.h")
    list(APPEND CORE_SOURCES ${ODBC_DB_SOURCES})
  endif()
  
elseif(TARGET_DATABASE STREQUAL "MYSQL")
  file(GLOB DB_SOURCES "core/database/mysql/*.cpp" "core/database/mysql/*.h")
  file(GLOB ODBC_DB_SOURCES "odbc/mysql/*.cpp" "odbc/mysql/*.h")
  list(APPEND CORE_SOURCES ${DB_SOURCES} ${ODBC_DB_SOURCES})
  
elseif(TARGET_DATABASE STREQUAL "SQLSERVER")
  file(GLOB DB_SOURCES "core/database/sqlserver/*.cpp" "core/database/sqlserver/*.h")
  file(GLOB ODBC_DB_SOURCES "odbc/sqlserver/*.cpp" "odbc/sqlserver/*.h")
  list(APPEND CORE_SOURCES ${DB_SOURCES} ${ODBC_DB_SOURCES})
  
else()
  message(FATAL_ERROR "Unknown TARGET_DATABASE: ${TARGET_DATABASE}")
endif()

# Add connection pool
list(APPEND CORE_SOURCES "core/database/connection_pool.cpp" "core/database/connection_pool.h")

# Add async transport
list(APPEND CORE_SOURCES 
  "core/transport/async_transport.h"
  "core/transport/thread_pool_transport.h"
  "core/transport/thread_pool_transport.cpp"
)

# Add ODBC layer
list(APPEND CORE_SOURCES
  "odbc/odbc_types.h"
  "odbc/odbc_handles.h"
  "odbc/odbc_handles.cpp"
  "odbc/odbc_api.h"
  "odbc/c_api_guard.h"
  "odbc/odbc_api.cpp"
  "odbc/connection_string.h"
  "odbc/connection_string.cpp"
  "odbc/result_types.h"
  "odbc/result_types.cpp"
  "odbc/text_data_converter.h"
  "odbc/text_data_converter.cpp"
  "odbc/testing_hooks.h"
  "odbc/unicode.h"
  "odbc/unicode.cpp"
)

if(WIN32)
  list(APPEND CORE_SOURCES "odbc/windows_registry.cpp" "odbc/windows_registry.h")
endif()

# Build static library for linking examples/tests
add_library(${PROJECT_NAME}_core STATIC ${CORE_SOURCES})
add_library(${PROJECT_NAME}::core ALIAS ${PROJECT_NAME}_core)

# Build shared library for ODBC Driver Manager
add_library(${PROJECT_NAME}_driver SHARED ${CORE_SOURCES})
set(_driver_exports_file "${CMAKE_CURRENT_SOURCE_DIR}/odbc/odbcpp.exports")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_driver_exports_file}")
file(STRINGS "${_driver_exports_file}" _driver_exports)
list(FILTER _driver_exports EXCLUDE REGEX "^[ \t]*(#.*)?$")
set(_validated_driver_exports "")
foreach(_export IN LISTS _driver_exports)
  string(STRIP "${_export}" _export)
  if(NOT _export MATCHES "^SQL[A-Za-z0-9_]+$")
    message(FATAL_ERROR "Invalid ODBC driver export: ${_export}")
  endif()
  if(_export IN_LIST _validated_driver_exports)
    message(FATAL_ERROR "Duplicate ODBC driver export: ${_export}")
  endif()
  list(APPEND _validated_driver_exports "${_export}")
endforeach()
list(LENGTH _validated_driver_exports _driver_export_count)
if(NOT _driver_export_count EQUAL 76)
  message(FATAL_ERROR
    "ODBC driver export allowlist must contain 76 entries, found ${_driver_export_count}")
endif()

if(WIN32)
  set(_driver_export_control "${CMAKE_CURRENT_BINARY_DIR}/odbcpp.def")
  file(WRITE "${_driver_export_control}" "LIBRARY odbcpp\nEXPORTS\n")
  foreach(_export IN LISTS _validated_driver_exports)
    file(APPEND "${_driver_export_control}" "  ${_export}\n")
  endforeach()
  target_sources(${PROJECT_NAME}_driver PRIVATE "${_driver_export_control}")
elseif(APPLE)
  set(_driver_export_control "${CMAKE_CURRENT_BINARY_DIR}/odbcpp.exports.macos")
  file(WRITE "${_driver_export_control}" "")
  foreach(_export IN LISTS _validated_driver_exports)
    file(APPEND "${_driver_export_control}" "_${_export}\n")
  endforeach()
  target_link_options(${PROJECT_NAME}_driver PRIVATE
    "LINKER:-exported_symbols_list,${_driver_export_control}")
  set_property(TARGET ${PROJECT_NAME}_driver APPEND PROPERTY
    LINK_DEPENDS "${_driver_export_control}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_driver_export_control "${CMAKE_CURRENT_BINARY_DIR}/odbcpp.exports.map")
  file(WRITE "${_driver_export_control}" "{\n  global:\n")
  foreach(_export IN LISTS _validated_driver_exports)
    file(APPEND "${_driver_export_control}" "    ${_export};\n")
  endforeach()
  file(APPEND "${_driver_export_control}" "  local: *;\n};\n")
  target_link_options(${PROJECT_NAME}_driver PRIVATE
    "LINKER:--version-script,${_driver_export_control}")
  set_property(TARGET ${PROJECT_NAME}_driver APPEND PROPERTY
    LINK_DEPENDS "${_driver_export_control}")
  if(ODBCPP_CRYPTO_LINKAGE STREQUAL "BUNDLED_STATIC")
    execute_process(
      COMMAND "${CMAKE_LINKER}" --version
      RESULT_VARIABLE _static_linker_result
      OUTPUT_VARIABLE ODBCPP_CRYPTO_STATIC_LINKER_IDENTITY
      ERROR_VARIABLE _static_linker_error
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _static_linker_result EQUAL 0 OR
       NOT ODBCPP_CRYPTO_STATIC_LINKER_IDENTITY MATCHES "^GNU ld ")
      message(FATAL_ERROR
        "Linux BUNDLED_STATIC evidence currently requires GNU ld: "
        "${_static_linker_error}${ODBCPP_CRYPTO_STATIC_LINKER_IDENTITY}")
    endif()
    string(REGEX MATCH "^[^\r\n]+"
      ODBCPP_CRYPTO_STATIC_LINKER_IDENTITY
      "${ODBCPP_CRYPTO_STATIC_LINKER_IDENTITY}")
    set(ODBCPP_CRYPTO_STATIC_LINK_MAP
        "${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-static-link.map")
    target_link_options(${PROJECT_NAME}_driver PRIVATE
      "LINKER:-Map,${ODBCPP_CRYPTO_STATIC_LINK_MAP}")
  endif()
endif()
set_target_properties(${PROJECT_NAME}_driver PROPERTIES
  OUTPUT_NAME "odbcpp"
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
)
# Configure static library
target_include_directories(${PROJECT_NAME}_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
    ${ODBC_INCLUDE_DIR}
)
target_include_directories(${PROJECT_NAME}_core SYSTEM PRIVATE
  ${SPDLOG_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME}_core 
  PUBLIC 
    OpenSSL::SSL 
    OpenSSL::Crypto
)

target_compile_definitions(${PROJECT_NAME}_core PRIVATE
  ODBCPP_ENABLE_${TARGET_DATABASE}=1
)

if(BUILD_TESTING)
  target_compile_definitions(${PROJECT_NAME}_core PUBLIC
    ODBCPP_ENABLE_TEST_HOOKS=1)
endif()

# Configure shared library (ODBC driver)
target_include_directories(${PROJECT_NAME}_driver
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
    ${ODBC_INCLUDE_DIR}
)
target_include_directories(${PROJECT_NAME}_driver SYSTEM PRIVATE
  ${SPDLOG_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME}_driver
  PRIVATE
    OpenSSL::SSL 
    OpenSSL::Crypto
)

target_compile_definitions(${PROJECT_NAME}_driver PRIVATE
  ODBCPP_ENABLE_${TARGET_DATABASE}=1
)

foreach(_target IN ITEMS ${PROJECT_NAME}_core ${PROJECT_NAME}_driver)
  get_target_property(_interface_definitions ${_target}
                      INTERFACE_COMPILE_DEFINITIONS)
  if(_interface_definitions AND
     _interface_definitions MATCHES
       "ODBCPP_(ENABLE_(POSTGRESQL|REDSHIFT|MYSQL|SQLSERVER)|TARGET_DATABASE)")
    message(FATAL_ERROR
      "${_target} exports private backend selection: ${_interface_definitions}")
  endif()
endforeach()

if(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE)
  target_compile_definitions(${PROJECT_NAME}_core PUBLIC
    ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
  target_compile_definitions(${PROJECT_NAME}_driver PUBLIC
    ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
endif()

if(WIN32)
  target_link_libraries(${PROJECT_NAME}_core PRIVATE ws2_32 crypt32 advapi32)
  target_link_libraries(${PROJECT_NAME}_driver PRIVATE ws2_32 crypt32 advapi32)
endif()

if(WIN32)
  enable_language(RC)
  add_library(odbcpp_setup SHARED odbc/setup/setup.cpp odbc/setup/setup_dialog.cpp odbc/setup/setup.rc odbc/setup/setup.def)
  target_link_libraries(odbcpp_setup PRIVATE odbccp32 odbc32 user32 comctl32)
  if(MSVC)
    # Windows SDK installer thunks reference legacy CRT formatting symbols.
    target_link_libraries(odbcpp_setup PRIVATE legacy_stdio_definitions)
  endif()
endif()

# ---- Compiler Settings ----
include(cmake/CompilerSettings.cmake)
apply_compiler_settings(${PROJECT_NAME}_core)
apply_compiler_settings(${PROJECT_NAME}_driver)

# ---- Examples ----
if(BUILD_EXAMPLES)
  include(cmake/Examples.cmake)
endif()

# ---- Tests ----
if(BUILD_TESTING)
  include(CTest)
  include(cmake/Tests.cmake)
endif()

# ---- Installation ----
include(cmake/Install.cmake)
