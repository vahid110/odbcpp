# Build-only qualification target; this module never supplies live admission.
if(NOT ODBCPP_NATIVE_PROVISIONED_AUTH_TESTS)
  return()
endif()
set(_native_test "${PROJECT_SOURCE_DIR}/tests/integration/it_redshift_native_auth.cpp")
if(NOT EXISTS "${_native_test}")
  message(FATAL_ERROR "Native provisioned auth selected, but the reviewed test source is absent")
endif()
if(NOT TARGET_DATABASE STREQUAL "REDSHIFT" OR
   NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
   NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
   NOT ODBCPP_CRYPTO_PROVIDER STREQUAL "OPENSSL" OR
   NOT ODBCPP_CRYPTO_LINKAGE STREQUAL "BUNDLED_STATIC")
  message(FATAL_ERROR "Native provisioned auth requires the reviewed Redshift Darwin arm64 static OpenSSL profile")
endif()
set(ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT "" CACHE PATH "Reviewed optional named-source foundation tree")
set(ODBCPP_NATIVE_AUTH_SDK_IMPORT "" CACHE FILEPATH "Reviewed pinned SDK import recipe")
foreach(_input ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT ODBCPP_NATIVE_AUTH_SDK_IMPORT)
  if(NOT IS_ABSOLUTE "${${_input}}" OR NOT EXISTS "${${_input}}")
    message(FATAL_ERROR "Native provisioned auth requires explicit existing absolute ${_input}")
  endif()
endforeach()
# Include executes CMake code. Accept only the reviewed recipe bytes, not a
# caller-supplied fingerprint or ambient installed-package discovery.
file(SHA256 "${ODBCPP_NATIVE_AUTH_SDK_IMPORT}" _recipe_hash)
if(NOT _recipe_hash STREQUAL "dd810b77e90777e9f9ff6cfddadaa699a02c6880f520c91cdb94fb387c7a3c00")
  message(FATAL_ERROR "Native provisioned auth SDK recipe fingerprint mismatch")
endif()
function(_native_auth_require_file relative expected)
  set(_path "${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/${relative}")
  if(NOT EXISTS "${_path}" OR IS_DIRECTORY "${_path}" OR IS_SYMLINK "${_path}")
    message(FATAL_ERROR "Native provisioned auth missing regular foundation input: ${relative}")
  endif()
  file(SHA256 "${_path}" _hash)
  if(NOT _hash STREQUAL "${expected}")
    message(FATAL_ERROR "Native provisioned auth foundation fingerprint mismatch: ${relative}")
  endif()
endfunction()
_native_auth_require_file("sdk/src/auth/aws/provisioned_native_owner.cpp" "3a2134d369a087edd0fc0454fc50c61a0ba67ab472ff45c8cdc4092e8ab3c630")
_native_auth_require_file("sdk/src/auth/aws/provisioned_native_http_ingress.cpp" "82c99919fffc20dd60b8ec7cbf76bdedb90981d9ee09ab16301e9c701fbf47c9")
_native_auth_require_file("sdk/src/auth/aws/named_profile_acquisition.cpp" "1f38a346cdbfca9134efb6a954be7e28ed8eac532c04b0e0aceb46e7c5fd4724")
_native_auth_require_file("sdk/src/auth/aws/native_body_capture.cpp" "16f5b705ee2e01ed0ae769b8f7c1b400d686df52aeff2fd66b36e45b1d8d44a3")
_native_auth_require_file("sdk/src/auth/aws/explicit_profile_source.cpp" "01b7557396773be11b8597b40beafa6297539ac787e0b6a388d8442b78866bf7")
_native_auth_require_file("sdk/src/auth/provisioned_query_transport.cpp" "5309f5ac3f01b917923e043f94d535bffd587cf849f96894cfccbd294a6de249")
_native_auth_require_file("sdk/internal/odbcpp/auth/aws/provisioned_native_owner.h" "285038ab1b2c36997c9122109022230b53a0463aa8036f31ff0ed7579f4ef677")
_native_auth_require_file("sdk/internal/odbcpp/auth/aws/provisioned_native_http_ingress.h" "57850aaa9b469d28ace2cc2e73cf3a7285c2cf0b1fc5f8844c9d473410689ab4")
_native_auth_require_file("sdk/internal/odbcpp/auth/aws/named_profile_acquisition.h" "41b9febc988da2d75dd7c1ade1b09570cf26be2b484991bb99e8ef80f2a28735")
_native_auth_require_file("sdk/internal/odbcpp/auth/aws/native_body_capture.h" "a4c75b92e8fa8a6f4315c87e8239f605d965b7cc6a76b30575f5b5cc7decf987")
_native_auth_require_file("sdk/internal/odbcpp/auth/aws/explicit_profile_source.h" "b32a01f891a45f563df4b4a178258c0fcfc22443574866b7eba47c22e08a8194")
_native_auth_require_file("sdk/internal/odbcpp/auth/provisioned_query_transport.h" "1e28e576c436a1a967bcc69677c2bcf02ce36cb82fa70b1d82214fafc6270e39")
if(NOT EXISTS "${OPENSSL_SSL_LIBRARY}")
  message(FATAL_ERROR "Native provisioned auth missing selected OPENSSL_SSL_LIBRARY")
endif()
file(SHA256 "${OPENSSL_SSL_LIBRARY}" _crypto_hash)
if(NOT _crypto_hash STREQUAL "23c6a50d6e01660db8ad58ed2f71305bf634970f044be2289b18d55ebc0370d9")
  message(FATAL_ERROR "Native provisioned auth requires reviewed OpenSSL3.6.5 archive: libssl.a")
endif()
if(NOT EXISTS "${OPENSSL_CRYPTO_LIBRARY}")
  message(FATAL_ERROR "Native provisioned auth missing selected OPENSSL_CRYPTO_LIBRARY")
endif()
file(SHA256 "${OPENSSL_CRYPTO_LIBRARY}" _crypto_hash)
if(NOT _crypto_hash STREQUAL "88981a4c6ce3c36127d12f6e7053b926d8a70ec1dbd0fa259113e6c8514a0583")
  message(FATAL_ERROR "Native provisioned auth requires reviewed OpenSSL3.6.5 archive: libcrypto.a")
endif()
foreach(_target odbcpp::core odbcpp_auth_core Threads::Threads GTest::gtest_main)
  if(NOT TARGET ${_target})
    message(FATAL_ERROR "Native provisioned auth missing prerequisite target: ${_target}")
  endif()
endforeach()
if(TARGET aws-cpp-sdk-redshift)
  message(FATAL_ERROR "Native provisioned auth refuses an already imported SDK target")
endif()
include("${ODBCPP_NATIVE_AUTH_SDK_IMPORT}")
if(NOT TARGET aws-cpp-sdk-redshift OR NOT SDK_FIXTURE_INCLUDES)
  message(FATAL_ERROR "Native provisioned auth incomplete SDK import closure")
endif()
foreach(_include IN LISTS SDK_FIXTURE_INCLUDES)
  if(NOT IS_ABSOLUTE "${_include}" OR NOT IS_DIRECTORY "${_include}")
    message(FATAL_ERROR "Native provisioned auth missing generated SDK include root")
  endif()
endforeach()
# Only the six optional headers are staged. Ordinary AuthCore headers retain
# their existing checked canonical stage; optional inputs are not installed.
set(_native_stage "${CMAKE_CURRENT_BINARY_DIR}/native-provisioned-auth-include")
file(REMOVE_RECURSE "${_native_stage}")
configure_file("${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/internal/odbcpp/auth/aws/provisioned_native_owner.h" "${_native_stage}/odbcpp/auth/aws/provisioned_native_owner.h" COPYONLY)
configure_file("${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/internal/odbcpp/auth/aws/provisioned_native_http_ingress.h" "${_native_stage}/odbcpp/auth/aws/provisioned_native_http_ingress.h" COPYONLY)
configure_file("${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/internal/odbcpp/auth/aws/named_profile_acquisition.h" "${_native_stage}/odbcpp/auth/aws/named_profile_acquisition.h" COPYONLY)
configure_file("${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/internal/odbcpp/auth/aws/native_body_capture.h" "${_native_stage}/odbcpp/auth/aws/native_body_capture.h" COPYONLY)
configure_file("${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/internal/odbcpp/auth/aws/explicit_profile_source.h" "${_native_stage}/odbcpp/auth/aws/explicit_profile_source.h" COPYONLY)
configure_file("${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/internal/odbcpp/auth/provisioned_query_transport.h" "${_native_stage}/odbcpp/auth/provisioned_query_transport.h" COPYONLY)
add_executable(it_redshift_native_auth EXCLUDE_FROM_ALL "${_native_test}"
  "${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/src/auth/aws/provisioned_native_owner.cpp"
  "${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/src/auth/aws/provisioned_native_http_ingress.cpp"
  "${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/src/auth/aws/named_profile_acquisition.cpp"
  "${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/src/auth/aws/native_body_capture.cpp"
  "${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/src/auth/aws/explicit_profile_source.cpp"
  "${ODBCPP_NATIVE_AUTH_FOUNDATION_ROOT}/sdk/src/auth/provisioned_query_transport.cpp"
 )
target_include_directories(it_redshift_native_auth PRIVATE
  "${_native_stage}" "${ODBCPP_INTERNAL_INCLUDE_auth}" "${ODBCPP_INTERNAL_INCLUDE_backend}")
target_include_directories(it_redshift_native_auth SYSTEM PRIVATE ${SDK_FIXTURE_INCLUDES})
target_compile_definitions(it_redshift_native_auth PRIVATE
  AWS_SDK_USE_CRT_HTTP HAVE_H2_CLIENT PLATFORM_APPLE AWS_ENABLE_DISPATCH_QUEUE
  AWS_ENABLE_KQUEUE AWS_SDK_VERSION_MAJOR=1 AWS_SDK_VERSION_MINOR=11 AWS_SDK_VERSION_PATCH=906)
target_link_libraries(it_redshift_native_auth PRIVATE
  odbcpp::core odbcpp_auth_core Threads::Threads GTest::gtest_main aws-cpp-sdk-redshift)
apply_compiler_settings(it_redshift_native_auth)
set_target_properties(it_redshift_native_auth PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
target_link_options(it_redshift_native_auth PRIVATE
  "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/native-provisioned-auth-link.map")
# No add_test/discovery/postbuild execution. A manually selected build is not
# SDK source permission, AWS/SQL coverage or runtime admission.
