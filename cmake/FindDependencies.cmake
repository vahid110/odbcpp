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