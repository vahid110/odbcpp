cmake_minimum_required(VERSION 3.20)
project(odbcpp VERSION 1.0.0 LANGUAGES CXX)

# ---- Project Options ----
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TESTING "Build tests" ON)
option(OPENSSL_USE_STATIC_LIBS "Link OpenSSL statically when available" ON)
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
message(STATUS "  Static OpenSSL: ${OPENSSL_USE_STATIC_LIBS}")
message(STATUS "  Examples: ${BUILD_EXAMPLES}")
message(STATUS "  Testing: ${BUILD_TESTING}")

# ---- Database Selection (Single Database Per Build) ----
set(TARGET_DATABASE "REDSHIFT" CACHE STRING "Target database for this build")
set_property(CACHE TARGET_DATABASE PROPERTY STRINGS "POSTGRESQL" "REDSHIFT" "MYSQL" "SQLSERVER")

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
  "core/util/*.h"
  "core/util/*.cpp"
)

set(CORE_SOURCES ${CORE_BASE_SOURCES})

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
  "odbc/sql_escape.h"
  "odbc/sql_escape.cpp"
  "odbc/text_data_converter.h"
  "odbc/text_data_converter.cpp"
  "odbc/testing_hooks.h"
  "odbc/unicode.h"
  "odbc/unicode.cpp"
)

# Build static library for linking examples/tests
add_library(${PROJECT_NAME}_core STATIC ${CORE_SOURCES})
add_library(${PROJECT_NAME}::core ALIAS ${PROJECT_NAME}_core)

# Build shared library for ODBC Driver Manager
add_library(${PROJECT_NAME}_driver SHARED ${CORE_SOURCES})
if(WIN32)
  target_sources(${PROJECT_NAME}_driver PRIVATE "odbc/odbcpp.def")
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

target_compile_definitions(${PROJECT_NAME}_core PUBLIC 
  ODBCPP_TARGET_DATABASE="${TARGET_DATABASE}"
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
  PUBLIC 
    OpenSSL::SSL 
    OpenSSL::Crypto
)

target_compile_definitions(${PROJECT_NAME}_driver PUBLIC 
  ODBCPP_TARGET_DATABASE="${TARGET_DATABASE}"
  ODBCPP_ENABLE_${TARGET_DATABASE}=1
)

if(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE)
  target_compile_definitions(${PROJECT_NAME}_core PUBLIC
    ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
  target_compile_definitions(${PROJECT_NAME}_driver PUBLIC
    ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
endif()

if(WIN32)
  target_link_libraries(${PROJECT_NAME}_core PRIVATE ws2_32 crypt32)
  target_link_libraries(${PROJECT_NAME}_driver PRIVATE ws2_32 crypt32)
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
