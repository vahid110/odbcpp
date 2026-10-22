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

# Build-profile custody only: do not let pkg-config's auxiliary static libraries
# escape the controlled prefix selected for the provider. Runtime SDK code has
# no dependency on this distro-specific, closed CI dependency recipe.
function(odbcpp_bind_static_crypto_dependencies)
  set(_inputs "")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ODBCPP_CRYPTO_LINKAGE STREQUAL "BUNDLED_STATIC")
    foreach(_target OpenSSL::Crypto OpenSSL::SSL)
      get_target_property(_libraries ${_target} INTERFACE_LINK_LIBRARIES)
      if(NOT _libraries)
        continue()
      endif()
      set(_bound "")
      foreach(_library IN LISTS _libraries)
        set(_archive "")
        if(_library STREQUAL ":libjitterentropy.a")
          set(_archive libjitterentropy.a)
        elseif(_library STREQUAL "ZLIB::ZLIB" OR _library STREQUAL "z")
          set(_archive libz.a)
        elseif(_library STREQUAL "zstd")
          set(_archive libzstd.a)
        elseif(NOT _library STREQUAL "OpenSSL::Crypto" AND
               NOT _library STREQUAL "Threads::Threads" AND
               NOT _library STREQUAL "${CMAKE_DL_LIBS}")
          message(FATAL_ERROR "Unrecognized static OpenSSL dependency: ${_library}")
        endif()
        if(_archive)
          set(_library "${ODBCPP_CRYPTO_ROOT_CANONICAL}/lib/${_archive}")
          if(NOT EXISTS "${_library}" OR IS_DIRECTORY "${_library}" OR IS_SYMLINK "${_library}")
            message(FATAL_ERROR "Missing regular controlled static dependency: ${_library}")
          endif()
          file(REAL_PATH "${_library}" _real)
          cmake_path(IS_PREFIX ODBCPP_CRYPTO_ROOT_CANONICAL "${_real}" NORMALIZE _inside)
          if(NOT _inside)
            message(FATAL_ERROR "Static auxiliary dependency escaped controlled root: ${_real}")
          endif()
          list(APPEND _inputs "${_library}")
        endif()
        list(APPEND _bound "${_library}")
      endforeach()
      set_property(TARGET ${_target} PROPERTY INTERFACE_LINK_LIBRARIES "${_bound}")
    endforeach()
  endif()
  list(REMOVE_DUPLICATES _inputs)
  set(ODBCPP_CRYPTO_STATIC_AUXILIARY_INPUTS "${_inputs}" PARENT_SCOPE)
endfunction()
