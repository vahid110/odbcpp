if(NOT ODBCPP_CRYPTO_LINKAGE MATCHES "^BUNDLED_(SHARED|STATIC)$")
  return()
endif()

function(odbcpp_require_crypto_path_under_root label candidate)
  if(NOT candidate OR candidate STREQUAL "optimized" OR candidate STREQUAL "debug")
    return()
  endif()
  if(NOT EXISTS "${candidate}")
    message(FATAL_ERROR "Resolved ${label} does not exist: ${candidate}")
  endif()
  file(REAL_PATH "${candidate}" _resolved)
  cmake_path(IS_PREFIX ODBCPP_CRYPTO_ROOT_CANONICAL "${_resolved}"
             NORMALIZE _inside_root)
  if(NOT _inside_root)
    message(FATAL_ERROR
      "Bundled crypto discovery escaped ODBCPP_CRYPTO_ROOT. Expected under "
      "${ODBCPP_CRYPTO_ROOT_CANONICAL}, but ${label} resolved to ${_resolved}")
  endif()
endfunction()

odbcpp_require_crypto_path_under_root("OpenSSL include directory"
                                      "${OPENSSL_INCLUDE_DIR}")
foreach(_library IN LISTS OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY)
  odbcpp_require_crypto_path_under_root("OpenSSL library" "${_library}")
endforeach()
