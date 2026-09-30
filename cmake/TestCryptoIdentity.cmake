if(NOT SOURCE_DIR OR NOT BINARY_DIR)
  message(FATAL_ERROR "SOURCE_DIR and BINARY_DIR are required")
endif()
set(_root "${BINARY_DIR}/crypto-header-identity-fixtures")
file(MAKE_DIRECTORY "${_root}/source" "${_root}/include/openssl")
file(WRITE "${_root}/source/CMakeLists.txt"
  "cmake_minimum_required(VERSION 3.20)\nproject(identity_fixture LANGUAGES CXX)\n"
  "include(\"${SOURCE_DIR}/cmake/VerifyCryptoIdentity.cmake\")\n")
file(WRITE "${_root}/include/openssl/crypto.h" "#pragma once\n")
function(check_identity name version marker expect_success)
  file(WRITE "${_root}/include/openssl/opensslv.h"
    "#define OPENSSL_VERSION_NUMBER 0x30000000L\n#define OPENSSL_VERSION_TEXT \"${version}\"\n${marker}\n")
  execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${_root}/source" -B "${_root}/build"
    "-DOPENSSL_INCLUDE_DIR=${_root}/include"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
  if(expect_success AND NOT _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly failed: ${_stdout}${_stderr}")
  elseif(NOT expect_success)
    if(_result EQUAL 0 OR NOT "${_stdout}${_stderr}" MATCHES
       "Selected crypto headers do not match provider OPENSSL")
      message(FATAL_ERROR "${name} did not reject the provider: ${_stdout}${_stderr}")
    endif()
  endif()
endfunction()
# Reuse one build directory to prove stale successful checks cannot mask changes.
check_identity(openssl "OpenSSL 3.0.0" "" true)
check_identity(aws_lc "OpenSSL 1.1.1" "#define OPENSSL_IS_AWSLC 1" false)
check_identity(boringssl "OpenSSL 1.1.1" "#define OPENSSL_IS_BORINGSSL 1" false)
check_identity(libressl "OpenSSL 1.1.1" "#define LIBRESSL_VERSION_NUMBER 1" false)
check_identity(unknown "Compatible TLS 1.0" "" false)
check_identity(old_openssl "OpenSSL 1.1.1" "#undef OPENSSL_VERSION_NUMBER\n#define OPENSSL_VERSION_NUMBER 0x10101000L" false)
file(RENAME "${_root}/include/openssl/crypto.h" "${_root}/include/openssl/crypto.saved")
check_identity(incomplete "OpenSSL 3.0.0" "" false)
file(RENAME "${_root}/include/openssl/crypto.saved" "${_root}/include/openssl/crypto.h")
check_identity(recovery "OpenSSL 3.0.0" "" true)
