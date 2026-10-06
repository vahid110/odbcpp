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
# Optional provider sources are versioned with this project. The SDK remains an
# explicitly pinned external build dependency; no ambient credential/package search.
set(ODBCPP_NATIVE_AUTH_SDK_IMPORT "" CACHE FILEPATH "Reviewed pinned SDK import recipe")
if(NOT IS_ABSOLUTE "${ODBCPP_NATIVE_AUTH_SDK_IMPORT}" OR
   NOT EXISTS "${ODBCPP_NATIVE_AUTH_SDK_IMPORT}")
  message(FATAL_ERROR "Native provisioned auth requires explicit existing absolute ODBCPP_NATIVE_AUTH_SDK_IMPORT")
endif()
# Include executes CMake code. Accept only the reviewed recipe bytes, not a
# caller-supplied fingerprint or ambient installed-package discovery.
file(SHA256 "${ODBCPP_NATIVE_AUTH_SDK_IMPORT}" _recipe_hash)
if(NOT _recipe_hash STREQUAL "dd810b77e90777e9f9ff6cfddadaa699a02c6880f520c91cdb94fb387c7a3c00")
  message(FATAL_ERROR "Native provisioned auth SDK recipe fingerprint mismatch")
endif()
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
# Only the eight optional headers are staged. Ordinary AuthCore headers retain
# their existing checked canonical stage; optional inputs are not installed.
set(_native_stage "${CMAKE_CURRENT_BINARY_DIR}/native-provisioned-auth-include")
file(REMOVE_RECURSE "${_native_stage}")
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/provisioned_native_owner.h" "${_native_stage}/odbcpp/auth/aws/provisioned_native_owner.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/provisioned_sdk_runtime.h" "${_native_stage}/odbcpp/auth/aws/provisioned_sdk_runtime.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/provisioned_native_http_ingress.h" "${_native_stage}/odbcpp/auth/aws/provisioned_native_http_ingress.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/named_profile_acquisition.h" "${_native_stage}/odbcpp/auth/aws/named_profile_acquisition.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/native_body_capture.h" "${_native_stage}/odbcpp/auth/aws/native_body_capture.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/explicit_profile_source.h" "${_native_stage}/odbcpp/auth/aws/explicit_profile_source.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/provisioned_query_transport.h" "${_native_stage}/odbcpp/auth/provisioned_query_transport.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/provisioned_pg_connector.h" "${_native_stage}/odbcpp/auth/aws/provisioned_pg_connector.h" COPYONLY)
# Reusable private optional provider module; AuthCore retains no AWS dependency.
add_library(odbcpp_auth_aws_provisioned STATIC EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/provisioned_native_owner.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/provisioned_sdk_runtime.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/provisioned_native_http_ingress.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/named_profile_acquisition.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/native_body_capture.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/explicit_profile_source.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/provisioned_query_transport.cpp")
target_compile_features(odbcpp_auth_aws_provisioned PRIVATE cxx_std_20)
target_include_directories(odbcpp_auth_aws_provisioned PRIVATE
  "${_native_stage}" "${ODBCPP_INTERNAL_INCLUDE_auth}")
target_include_directories(odbcpp_auth_aws_provisioned SYSTEM PRIVATE ${SDK_FIXTURE_INCLUDES})
target_compile_definitions(odbcpp_auth_aws_provisioned PRIVATE
  AWS_SDK_USE_CRT_HTTP HAVE_H2_CLIENT PLATFORM_APPLE AWS_ENABLE_DISPATCH_QUEUE
  AWS_ENABLE_KQUEUE AWS_SDK_VERSION_MAJOR=1 AWS_SDK_VERSION_MINOR=11 AWS_SDK_VERSION_PATCH=906)
target_link_libraries(odbcpp_auth_aws_provisioned PRIVATE
  odbcpp_auth_core Threads::Threads aws-cpp-sdk-redshift)
apply_compiler_settings(odbcpp_auth_aws_provisioned)

# Optional provider-to-session composition keeps driver dependencies out of
# both provider-free AuthCore and the acquisition-only AWS module.
add_library(odbcpp_auth_aws_provisioned_pg STATIC EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/sdk/src/adapters/provisioned_pg_connector.cpp")
target_compile_features(odbcpp_auth_aws_provisioned_pg PRIVATE cxx_std_20)
target_include_directories(odbcpp_auth_aws_provisioned_pg PRIVATE
  "${_native_stage}" "${ODBCPP_INTERNAL_INCLUDE_auth}"
  "${ODBCPP_INTERNAL_INCLUDE_backend}" "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}")
target_include_directories(odbcpp_auth_aws_provisioned_pg SYSTEM PRIVATE ${SDK_FIXTURE_INCLUDES})
target_compile_definitions(odbcpp_auth_aws_provisioned_pg PRIVATE
  AWS_SDK_USE_CRT_HTTP HAVE_H2_CLIENT PLATFORM_APPLE AWS_ENABLE_DISPATCH_QUEUE
  AWS_ENABLE_KQUEUE AWS_SDK_VERSION_MAJOR=1 AWS_SDK_VERSION_MINOR=11 AWS_SDK_VERSION_PATCH=906)
target_link_libraries(odbcpp_auth_aws_provisioned_pg PRIVATE
  odbcpp_auth_aws_provisioned odbcpp::core odbcpp_auth_pg_consumer)
apply_compiler_settings(odbcpp_auth_aws_provisioned_pg)

# Explicit build-only consumers. Neither native nor SDK fake tests run through
# ordinary CTest; fake runtime uses the separately selected network-denied path.
add_executable(it_redshift_native_auth EXCLUDE_FROM_ALL "${_native_test}")
add_executable(test_named_profile_acquisition EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/tests/unit/test_named_profile_acquisition.cpp")
add_executable(test_provisioned_native_owner EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/tests/unit/test_provisioned_native_owner.cpp")
add_executable(test_provisioned_pg_connector EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/tests/unit/test_provisioned_pg_connector.cpp")
add_executable(test_provisioned_sdk_runtime EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/tests/unit/test_provisioned_sdk_runtime.cpp")
foreach(_consumer it_redshift_native_auth test_named_profile_acquisition test_provisioned_native_owner test_provisioned_pg_connector test_provisioned_sdk_runtime)
  target_include_directories(${_consumer} PRIVATE
    "${_native_stage}" "${ODBCPP_INTERNAL_INCLUDE_auth}")
  target_include_directories(${_consumer} SYSTEM PRIVATE ${SDK_FIXTURE_INCLUDES})
  target_compile_definitions(${_consumer} PRIVATE
    AWS_SDK_USE_CRT_HTTP HAVE_H2_CLIENT PLATFORM_APPLE AWS_ENABLE_DISPATCH_QUEUE
    AWS_ENABLE_KQUEUE AWS_SDK_VERSION_MAJOR=1 AWS_SDK_VERSION_MINOR=11 AWS_SDK_VERSION_PATCH=906)
  target_link_libraries(${_consumer} PRIVATE odbcpp_auth_aws_provisioned GTest::gtest_main)
  apply_compiler_settings(${_consumer})
  set_target_properties(${_consumer} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endforeach()
target_include_directories(it_redshift_native_auth PRIVATE "${ODBCPP_INTERNAL_INCLUDE_backend}")
target_include_directories(it_redshift_native_auth PRIVATE "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}")
target_include_directories(test_provisioned_pg_connector PRIVATE "${ODBCPP_INTERNAL_INCLUDE_backend}")
target_link_libraries(test_provisioned_pg_connector PRIVATE odbcpp_auth_aws_provisioned_pg)
target_link_libraries(it_redshift_native_auth PRIVATE odbcpp::core odbcpp_auth_pg_consumer odbcpp_auth_aws_provisioned_pg)
target_link_options(it_redshift_native_auth PRIVATE
  "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/native-provisioned-auth-link.map")
# No add_test/discovery/postbuild execution. A manually selected build is not
# SDK source permission, AWS/SQL coverage or runtime admission.
