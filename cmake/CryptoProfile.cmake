if(POLICY CMP0057)
  cmake_policy(SET CMP0057 NEW)
endif()

if(WIN32)
  set(_odbcpp_default_crypto_linkage BUNDLED_SHARED)
else()
  set(_odbcpp_default_crypto_linkage SYSTEM_SHARED)
endif()

set(ODBCPP_CRYPTO_PROVIDER "OPENSSL" CACHE STRING
    "Cryptography provider used by this driver artifact")
set_property(CACHE ODBCPP_CRYPTO_PROVIDER PROPERTY STRINGS OPENSSL AWS_LC)
set(ODBCPP_CRYPTO_LINKAGE "${_odbcpp_default_crypto_linkage}" CACHE STRING
    "Cryptography dependency origin and linkage")
set_property(CACHE ODBCPP_CRYPTO_LINKAGE PROPERTY STRINGS
             SYSTEM_SHARED BUNDLED_SHARED BUNDLED_STATIC)
set(ODBCPP_CRYPTO_ROOT "" CACHE PATH
    "Exact dependency prefix for bundled cryptography profiles")

if(DEFINED CACHE{OPENSSL_USE_STATIC_LIBS} AND
   NOT DEFINED CACHE{ODBCPP_CRYPTO_PROFILE_FINGERPRINT})
  message(FATAL_ERROR
    "OPENSSL_USE_STATIC_LIBS is no longer a product option; use "
    "ODBCPP_CRYPTO_LINKAGE with an explicit crypto profile")
endif()

set(_odbcpp_crypto_providers OPENSSL AWS_LC)
if(NOT ODBCPP_CRYPTO_PROVIDER IN_LIST _odbcpp_crypto_providers)
  message(FATAL_ERROR
    "Unknown ODBCPP_CRYPTO_PROVIDER=${ODBCPP_CRYPTO_PROVIDER}; expected "
    "OPENSSL (implemented) or AWS_LC (planned)")
endif()
if(ODBCPP_CRYPTO_PROVIDER STREQUAL "AWS_LC")
  message(FATAL_ERROR
    "ODBCPP_CRYPTO_PROVIDER=AWS_LC is planned but not implemented or qualified")
endif()

set(_odbcpp_crypto_linkages SYSTEM_SHARED BUNDLED_SHARED BUNDLED_STATIC)
if(NOT ODBCPP_CRYPTO_LINKAGE IN_LIST _odbcpp_crypto_linkages)
  message(FATAL_ERROR
    "Unknown ODBCPP_CRYPTO_LINKAGE=${ODBCPP_CRYPTO_LINKAGE}; expected "
    "SYSTEM_SHARED, BUNDLED_SHARED, or BUNDLED_STATIC")
endif()

if(ODBCPP_CRYPTO_LINKAGE STREQUAL "SYSTEM_SHARED")
  if(ODBCPP_CRYPTO_ROOT)
    message(FATAL_ERROR
      "ODBCPP_CRYPTO_ROOT is not allowed with SYSTEM_SHARED")
  endif()
  set(OPENSSL_USE_STATIC_LIBS OFF CACHE BOOL
      "Internal FindOpenSSL hint selected by ODBCPP_CRYPTO_LINKAGE" FORCE)
else()
  if(NOT ODBCPP_CRYPTO_ROOT)
    message(FATAL_ERROR
      "ODBCPP_CRYPTO_ROOT is required for ${ODBCPP_CRYPTO_LINKAGE}")
  endif()
  if(NOT IS_ABSOLUTE "${ODBCPP_CRYPTO_ROOT}" OR
     NOT IS_DIRECTORY "${ODBCPP_CRYPTO_ROOT}")
    message(FATAL_ERROR
      "ODBCPP_CRYPTO_ROOT must be an existing absolute directory")
  endif()
  file(REAL_PATH "${ODBCPP_CRYPTO_ROOT}" ODBCPP_CRYPTO_ROOT_CANONICAL)
  set(OPENSSL_ROOT_DIR "${ODBCPP_CRYPTO_ROOT_CANONICAL}" CACHE PATH
      "Internal provider discovery prefix" FORCE)
  if(ODBCPP_CRYPTO_LINKAGE STREQUAL "BUNDLED_STATIC")
    set(OPENSSL_USE_STATIC_LIBS ON CACHE BOOL
        "Internal FindOpenSSL hint selected by ODBCPP_CRYPTO_LINKAGE" FORCE)
  else()
    set(OPENSSL_USE_STATIC_LIBS OFF CACHE BOOL
        "Internal FindOpenSSL hint selected by ODBCPP_CRYPTO_LINKAGE" FORCE)
  endif()
endif()

set(_odbcpp_crypto_fingerprint
    "${ODBCPP_CRYPTO_PROVIDER}|${ODBCPP_CRYPTO_LINKAGE}|${ODBCPP_CRYPTO_ROOT}")
if(DEFINED CACHE{ODBCPP_CRYPTO_PROFILE_FINGERPRINT} AND
   NOT ODBCPP_CRYPTO_PROFILE_FINGERPRINT STREQUAL _odbcpp_crypto_fingerprint)
  message(FATAL_ERROR
    "The crypto profile changed in an existing build directory. Use a fresh "
    "build directory to prevent stale dependency discovery.")
endif()
set(ODBCPP_CRYPTO_PROFILE_FINGERPRINT "${_odbcpp_crypto_fingerprint}"
    CACHE INTERNAL "Configured cryptography profile fingerprint")
mark_as_advanced(OPENSSL_USE_STATIC_LIBS OPENSSL_ROOT_DIR)
