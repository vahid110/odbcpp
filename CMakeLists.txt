cmake_minimum_required(VERSION 3.20)
project(redshift_odbcpp_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_EXAMPLES "Build examples" ON)
option(OPENSSL_USE_STATIC_LIBS "Link OpenSSL statically when available" OFF)

# --- OpenSSL discovery ---
# Users can override with -DOPENSSL_ROOT_DIR=/path/to/openssl
# Common macOS Homebrew prefixes are hinted automatically.
if(APPLE)
  foreach(_ossl_prefix
          /opt/homebrew/opt/openssl@3
          /usr/local/opt/openssl@3
          /opt/homebrew/opt/openssl
          /usr/local/opt/openssl)
    if(EXISTS "${_ossl_prefix}")
      list(APPEND CMAKE_PREFIX_PATH "${_ossl_prefix}")
    endif()
  endforeach()
endif()

# Primary discovery via CMake's OpenSSL package
find_package(OpenSSL REQUIRED)

message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
message(STATUS "  SSL lib: ${OPENSSL_SSL_LIBRARY}")
message(STATUS "  Crypto lib: ${OPENSSL_CRYPTO_LIBRARY}")
message(STATUS "  Include dir: ${OPENSSL_INCLUDE_DIR}")

# --- Fallback if OpenSSL::SSL/::Crypto targets are not defined ---
if(NOT TARGET OpenSSL::SSL)
  add_library(OpenSSL::SSL INTERFACE IMPORTED)
  set_target_properties(OpenSSL::SSL PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${OPENSSL_SSL_LIBRARY}"
  )
endif()

if(NOT TARGET OpenSSL::Crypto)
  add_library(OpenSSL::Crypto INTERFACE IMPORTED)
  set_target_properties(OpenSSL::Crypto PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${OPENSSL_CRYPTO_LIBRARY}"
  )
endif()

# --- Core library ---
add_library(rs_core STATIC
  core/transport/ITransport.h
  core/transport/SocketTransport.h
  core/transport/SocketTransport.cpp
  core/transport/TLSTransport.h
  core/transport/TLSTransport.cpp
  core/util/Deadline.h
  core/util/Errors.h
  core/util/Platform.h
  core/pgwire/Messages.h
  core/engine/PgConnection.h
  core/engine/PgConnection.cpp
)

if (MSVC)
  target_compile_options(rs_core PRIVATE /W4 /WX)
else()
  target_compile_options(rs_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if (WIN32)
  target_link_libraries(rs_core PRIVATE ws2_32 crypt32)
endif()

# Link OpenSSL and expose include dirs to rs_core
target_link_libraries(rs_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
target_include_directories(rs_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OPENSSL_INCLUDE_DIR}
)

# --- Example ---
if (BUILD_EXAMPLES)
  add_executable(connect_example examples/connect_example.cpp)
  target_link_libraries(connect_example PRIVATE rs_core)
  add_executable(pg_handshake_example examples/pg_handshake_example.cpp)
  target_link_libraries(pg_handshake_example PRIVATE rs_core)
  add_executable(query_example examples/query_example.cpp)
  target_link_libraries(query_example PRIVATE rs_core)
  add_executable(extended_query_example examples/extended_query_example.cpp)
  target_link_libraries(extended_query_example PRIVATE rs_core)
endif()
