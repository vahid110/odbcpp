# ---- OpenSSL Discovery ----
if(APPLE)
  set(_openssl_paths
    /opt/homebrew/opt/openssl@3
    /usr/local/opt/openssl@3
    /opt/homebrew/opt/openssl
    /usr/local/opt/openssl
  )
  
  foreach(_path ${_openssl_paths})
    if(EXISTS "${_path}")
      list(APPEND CMAKE_PREFIX_PATH "${_path}")
    endif()
  endforeach()
endif()

find_package(OpenSSL REQUIRED)

message(STATUS "OpenSSL ${OPENSSL_VERSION} found")
message(STATUS "  Include: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "  Libraries: ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY}")

# ---- Fallback for older CMake/OpenSSL combinations ----
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

# ---- Logging ----
# Use spdlog's header-only distribution so the installed static ODBCPP target
# does not expose a third-party link dependency.
include(FetchContent)
FetchContent_Declare(
  spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip
  URL_HASH SHA256=b11912a82d149792fef33fabd0503b13d54aeac25c1464755461d4108ea71fc2
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(spdlog)
if(NOT spdlog_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(spdlog)
  if(POLICY CMP0169)
    cmake_policy(POP)
  endif()
endif()
set(SPDLOG_INCLUDE_DIR "${spdlog_SOURCE_DIR}/include")
