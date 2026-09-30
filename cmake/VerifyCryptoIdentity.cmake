# Compile the selected headers rather than trusting FindOpenSSL's compatibility
# version. This establishes header identity only, not loaded-library identity.
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)
function(odbcpp_verify_crypto_header_identity)
  file(REAL_PATH "${OPENSSL_INCLUDE_DIR}" _include_root)
  foreach(_header IN ITEMS crypto.h opensslv.h)
    set(_candidate "${OPENSSL_INCLUDE_DIR}/openssl/${_header}")
    if(NOT EXISTS "${_candidate}")
      message(FATAL_ERROR "Selected crypto headers do not match provider OPENSSL: missing ${_header}")
    endif()
    file(REAL_PATH "${_candidate}" _header_path)
    cmake_path(IS_PREFIX _include_root "${_header_path}" NORMALIZE _inside)
    if(NOT _inside)
      message(FATAL_ERROR "Selected crypto headers do not match provider OPENSSL: escaped include root")
    endif()
  endforeach()
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_INCLUDES "${OPENSSL_INCLUDE_DIR}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  unset(ODBCPP_OPENSSL_HEADERS_MATCH CACHE)
  check_cxx_source_compiles([=[
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#if defined(OPENSSL_IS_AWSLC) || defined(OPENSSL_IS_BORINGSSL) || defined(LIBRESSL_VERSION_NUMBER)
#error The selected headers belong to another provider
#endif
#if !defined(OPENSSL_VERSION_NUMBER) || OPENSSL_VERSION_NUMBER < 0x30000000L
#error OpenSSL 3.0 or later is required
#endif
constexpr char version[] = OPENSSL_VERSION_TEXT;
static_assert(sizeof(version) > 8 && version[0] == 'O' && version[1] == 'p' &&
              version[2] == 'e' && version[3] == 'n' && version[4] == 'S' &&
              version[5] == 'S' && version[6] == 'L' && version[7] == ' ',
              "The selected headers do not identify OpenSSL");
int main() { return 0; }
]=] ODBCPP_OPENSSL_HEADERS_MATCH)
  cmake_pop_check_state()
  if(NOT ODBCPP_OPENSSL_HEADERS_MATCH)
    message(FATAL_ERROR "Selected crypto headers do not match provider OPENSSL")
  endif()
endfunction()
odbcpp_verify_crypto_header_identity()
