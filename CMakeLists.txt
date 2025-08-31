cmake_minimum_required(VERSION 3.20)
project(odbcpp VERSION 1.0.0 LANGUAGES CXX)

# ---- Project Options ----
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TESTING "Build tests" ON)
option(OPENSSL_USE_STATIC_LIBS "Link OpenSSL statically when available" OFF)

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

# ---- Core Library ----
# Base sources (always included)
file(GLOB CORE_BASE_SOURCES
  "core/database/*.cpp"
  "core/database/*.h"
  "core/transport/*.cpp"
  "core/transport/*.h"
  "core/util/*.h"
)

set(CORE_SOURCES ${CORE_BASE_SOURCES})

# Add database-specific sources based on TARGET_DATABASE
if(TARGET_DATABASE STREQUAL "POSTGRESQL" OR TARGET_DATABASE STREQUAL "REDSHIFT")
  file(GLOB DB_SOURCES "core/database/postgres/*.cpp" "core/database/postgres/*.h")
  list(APPEND CORE_SOURCES ${DB_SOURCES})
elseif(TARGET_DATABASE STREQUAL "MYSQL")
  file(GLOB DB_SOURCES "core/database/mysql/*.cpp" "core/database/mysql/*.h")
  list(APPEND CORE_SOURCES ${DB_SOURCES})
elseif(TARGET_DATABASE STREQUAL "SQLSERVER")
  file(GLOB DB_SOURCES "core/database/sqlserver/*.cpp" "core/database/sqlserver/*.h")
  list(APPEND CORE_SOURCES ${DB_SOURCES})
else()
  message(FATAL_ERROR "Unknown TARGET_DATABASE: ${TARGET_DATABASE}")
endif()

# Add connection pool
list(APPEND CORE_SOURCES "core/database/connection_pool.cpp" "core/database/connection_pool.h")

add_library(${PROJECT_NAME}_core STATIC ${CORE_SOURCES})
add_library(${PROJECT_NAME}::core ALIAS ${PROJECT_NAME}_core)

target_include_directories(${PROJECT_NAME}_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(${PROJECT_NAME}_core 
  PUBLIC 
    OpenSSL::SSL 
    OpenSSL::Crypto
)

# Define target database
target_compile_definitions(${PROJECT_NAME}_core PUBLIC 
  ODBCPP_TARGET_DATABASE="${TARGET_DATABASE}"
  ODBCPP_ENABLE_${TARGET_DATABASE}=1
)

if(WIN32)
  target_link_libraries(${PROJECT_NAME}_core PRIVATE ws2_32 crypt32)
endif()

# ---- Compiler Settings ----
include(cmake/CompilerSettings.cmake)
apply_compiler_settings(${PROJECT_NAME}_core)

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