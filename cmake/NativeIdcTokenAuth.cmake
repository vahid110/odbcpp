# Build-only qualification target for the native Identity Center token owner;
# this module never supplies live admission. It reuses the reviewed pinned AWS
# SDK import and the shared native-AWS SDK runtime; the optional native-IdP
# token acquisition is a Redshift-serverless feature, not tied to the provisioned
# static-OpenSSL profile.
if(NOT ODBCPP_REDSHIFT_NATIVE_IDC_TOKEN_TESTS)
  return()
endif()
if(NOT TARGET_DATABASE STREQUAL "REDSHIFT")
  message(FATAL_ERROR "Native IdC token auth requires REDSHIFT")
endif()
foreach(_target odbcpp::core odbcpp_auth_core Threads::Threads GTest::gtest_main)
  if(NOT TARGET ${_target})
    message(FATAL_ERROR "Native IdC token auth missing prerequisite target: ${_target}")
  endif()
endforeach()
if(TARGET aws-cpp-sdk-redshift-serverless)
  message(FATAL_ERROR "Native IdC token auth refuses an already imported SDK target")
endif()
include("${PROJECT_SOURCE_DIR}/cmake/NativeAwsSdkImport.cmake")
if(NOT TARGET aws-cpp-sdk-redshift-serverless OR NOT SDK_FIXTURE_INCLUDES)
  message(FATAL_ERROR "Native IdC token auth incomplete SDK import closure")
endif()
foreach(_include IN LISTS SDK_FIXTURE_INCLUDES)
  if(NOT IS_ABSOLUTE "${_include}" OR NOT IS_DIRECTORY "${_include}")
    message(FATAL_ERROR "Native IdC token auth missing generated SDK include root")
  endif()
endforeach()
# Stage the optional headers; ordinary AuthCore headers retain their checked
# canonical stage. Optional inputs are not installed.
set(_native_stage "${CMAKE_CURRENT_BINARY_DIR}/native-idc-token-auth-include")
file(REMOVE_RECURSE "${_native_stage}")
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/idc_token_owner.h" "${_native_stage}/odbcpp/auth/aws/idc_token_owner.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/provisioned_native_owner.h" "${_native_stage}/odbcpp/auth/aws/provisioned_native_owner.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/provisioned_sdk_runtime.h" "${_native_stage}/odbcpp/auth/aws/provisioned_sdk_runtime.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/provisioned_native_http_ingress.h" "${_native_stage}/odbcpp/auth/aws/provisioned_native_http_ingress.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/named_profile_acquisition.h" "${_native_stage}/odbcpp/auth/aws/named_profile_acquisition.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/native_body_capture.h" "${_native_stage}/odbcpp/auth/aws/native_body_capture.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/explicit_profile_source.h" "${_native_stage}/odbcpp/auth/aws/explicit_profile_source.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/sts_serverless_fake_http.h" "${_native_stage}/odbcpp/auth/aws/sts_serverless_fake_http.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/aws/browser_idc_owner.h" "${_native_stage}/odbcpp/auth/aws/browser_idc_owner.h" COPYONLY)
configure_file("${PROJECT_SOURCE_DIR}/sdk/internal/odbcpp/auth/provisioned_query_transport.h" "${_native_stage}/odbcpp/auth/provisioned_query_transport.h" COPYONLY)
# Reusable private optional provider module; AuthCore retains no AWS dependency.
add_library(odbcpp_auth_aws_idc STATIC EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/idc_token_owner.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/browser_idc_owner.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/provisioned_native_owner.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/provisioned_sdk_runtime.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/provisioned_native_http_ingress.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/named_profile_acquisition.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/native_body_capture.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/explicit_profile_source.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/aws/sts_serverless_fake_http.cpp"
  "${PROJECT_SOURCE_DIR}/sdk/src/auth/provisioned_query_transport.cpp")
target_compile_features(odbcpp_auth_aws_idc PRIVATE cxx_std_20)
target_include_directories(odbcpp_auth_aws_idc PRIVATE
  "${_native_stage}" "${ODBCPP_INTERNAL_INCLUDE_auth}")
target_include_directories(odbcpp_auth_aws_idc SYSTEM PRIVATE ${SDK_FIXTURE_INCLUDES})
target_compile_definitions(odbcpp_auth_aws_idc PRIVATE
  AWS_SDK_USE_CRT_HTTP HAVE_H2_CLIENT PLATFORM_APPLE AWS_ENABLE_DISPATCH_QUEUE
  AWS_ENABLE_KQUEUE AWS_SDK_VERSION_MAJOR=1 AWS_SDK_VERSION_MINOR=11 AWS_SDK_VERSION_PATCH=906)
target_link_libraries(odbcpp_auth_aws_idc PRIVATE
  odbcpp_auth_core Threads::Threads aws-cpp-sdk-redshift aws-cpp-sdk-redshift-serverless
  aws-cpp-sdk-sso-oidc OpenSSL::Crypto)
apply_compiler_settings(odbcpp_auth_aws_idc)

# macOS browser launch (LaunchServices + CoreFoundation).
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_libraries(odbcpp_auth_aws_idc PRIVATE "-framework CoreFoundation" "-framework CoreServices")
endif()

# Offline owner test (fake HTTP) and the separately admitted live login test.
add_executable(test_idc_token_owner EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/tests/unit/test_idc_token_owner.cpp")
add_executable(test_browser_idc_owner EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/tests/unit/test_browser_idc_owner.cpp")
add_executable(it_redshift_native_idc_token_live EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/tests/integration/it_redshift_native_idc_token_live.cpp")
add_executable(it_redshift_browser_idc_live EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/tests/integration/it_redshift_browser_idc_live.cpp")
foreach(_consumer test_idc_token_owner test_browser_idc_owner it_redshift_native_idc_token_live it_redshift_browser_idc_live)
  target_include_directories(${_consumer} PRIVATE
    "${_native_stage}" "${ODBCPP_INTERNAL_INCLUDE_auth}")
  target_include_directories(${_consumer} SYSTEM PRIVATE ${SDK_FIXTURE_INCLUDES})
  target_compile_definitions(${_consumer} PRIVATE
    AWS_SDK_USE_CRT_HTTP HAVE_H2_CLIENT PLATFORM_APPLE AWS_ENABLE_DISPATCH_QUEUE
    AWS_ENABLE_KQUEUE AWS_SDK_VERSION_MAJOR=1 AWS_SDK_VERSION_MINOR=11 AWS_SDK_VERSION_PATCH=906)
  target_link_libraries(${_consumer} PRIVATE odbcpp_auth_aws_idc GTest::gtest_main)
  apply_compiler_settings(${_consumer})
  set_target_properties(${_consumer} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endforeach()
# The live logins also drive the compiled provider session (resolve -> connect).
target_link_libraries(it_redshift_native_idc_token_live PRIVATE odbcpp::core)
target_link_libraries(it_redshift_browser_idc_live PRIVATE odbcpp::core)
# Offline fake-HTTP tests run under the reviewed network-denied marker only.
add_test(NAME test_idc_token_owner COMMAND test_idc_token_owner)
set_tests_properties(test_idc_token_owner PROPERTIES
  ENVIRONMENT "ODBCPP_AUTH_SDK_OFFLINE_FIXTURE=synthetic-fake-http-only"
  LABELS "unit;redshift;idc" TIMEOUT 60)
add_test(NAME test_browser_idc_owner COMMAND test_browser_idc_owner)
set_tests_properties(test_browser_idc_owner PROPERTIES
  ENVIRONMENT "ODBCPP_AUTH_SDK_OFFLINE_FIXTURE=synthetic-fake-http-only"
  LABELS "unit;redshift;idc" TIMEOUT 60)
# No add_test for the live binaries: a manually selected build is not SDK source
# permission, AWS/SQL coverage or runtime admission.
