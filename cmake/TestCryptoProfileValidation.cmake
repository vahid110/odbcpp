if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

function(expect_profile_failure name expected)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" ${ARGN}
            -P "${SOURCE_DIR}/cmake/CryptoProfile.cmake"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(_result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly succeeded")
  endif()
  set(_output "${_stdout}\n${_stderr}")
  if(NOT _output MATCHES "${expected}")
    message(FATAL_ERROR "${name} failed unexpectedly:\n${_output}")
  endif()
endfunction()

expect_profile_failure(unknown-provider "Unknown ODBCPP_CRYPTO_PROVIDER"
  -DODBCPP_CRYPTO_PROVIDER=BOGUS)
expect_profile_failure(lowercase-provider "Unknown ODBCPP_CRYPTO_PROVIDER"
  -DODBCPP_CRYPTO_PROVIDER=openssl)
expect_profile_failure(unimplemented-provider
  "AWS_LC is planned but not implemented or qualified"
  -DODBCPP_CRYPTO_PROVIDER=AWS_LC)
expect_profile_failure(unknown-linkage "Unknown ODBCPP_CRYPTO_LINKAGE"
  -DODBCPP_CRYPTO_LINKAGE=STATIC)
expect_profile_failure(system-root
  "ODBCPP_CRYPTO_ROOT is not allowed with SYSTEM_SHARED"
  -DODBCPP_CRYPTO_LINKAGE=SYSTEM_SHARED
  -DODBCPP_CRYPTO_ROOT=${SOURCE_DIR})
expect_profile_failure(bundled-shared-root
  "ODBCPP_CRYPTO_ROOT is required for BUNDLED_SHARED"
  -DODBCPP_CRYPTO_LINKAGE=BUNDLED_SHARED)
expect_profile_failure(bundled-static-root
  "ODBCPP_CRYPTO_ROOT is required for BUNDLED_STATIC"
  -DODBCPP_CRYPTO_LINKAGE=BUNDLED_STATIC)
expect_profile_failure(invalid-root
  "ODBCPP_CRYPTO_ROOT must be an existing absolute directory"
  -DODBCPP_CRYPTO_LINKAGE=BUNDLED_STATIC
  -DODBCPP_CRYPTO_ROOT=relative)
expect_profile_failure(legacy-option
  "OPENSSL_USE_STATIC_LIBS is no longer a product option"
  -DOPENSSL_USE_STATIC_LIBS=OFF)
