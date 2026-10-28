# ---- Google Test Setup ----
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---- Test Discovery and Setup ----
# Auth-core units link only odbcpp_auth_core, never the driver core.
set(ODBCPP_AUTH_CORE_UNIT_TESTS
  test_auth_core test_temporary_db_validity test_issuer_timestamp
  test_aws_db_response_fields test_bounded_response_stream test_aws_db_json_response
  test_aws_db_xml_response test_redshift_serverless_response
  test_redshift_provisioned_response test_redshift_withiam_response
  test_sts_assume_role_response test_auth_raw_response_projection
  test_checked_response_boundary test_checked_response_stream
  test_checked_aws_db_json_response test_response_operation
  test_checked_aws_db_xml_response)
foreach(_test IN LISTS ODBCPP_AUTH_CORE_UNIT_TESTS)
  set(ODBCPP_TEST_LIBRARIES_${_test} odbcpp_auth_core)
endforeach()
set(ODBCPP_TEST_LIBRARIES_test_pg_credential_consumer odbcpp_auth_pg_consumer)

# Builds one GoogleTest image into tests/ and registers it. The image links the
# driver core unless ODBCPP_TEST_LIBRARIES_<name> names its libraries instead;
# other per-test additions follow the discovery loops.
function(add_test_executable test_name test_file)
  add_executable(${test_name} ${test_file})
  if(DEFINED ODBCPP_TEST_LIBRARIES_${test_name})
    target_link_libraries(${test_name} PRIVATE ${ODBCPP_TEST_LIBRARIES_${test_name}} GTest::gtest_main)
  else()
    target_link_libraries(${test_name} PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
  endif()
  apply_compiler_settings(${test_name})
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_target_properties(${test_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endfunction()

option(ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP_LIVE_TESTS "Build/register separately admitted single-key relationship DM assertions" OFF)
option(ODBCPP_REDSHIFT_PACKAGE_LIVE_TESTS "Build separately admitted installed Redshift Driver Manager tests" OFF)
option(ODBCPP_REDSHIFT_PACKAGE_IMPORT_LIVE_TESTS "Build separately admitted installed Redshift import/refresh tests" OFF)
option(ODBCPP_REDSHIFT_OFFICIAL_IMPORT_LIVE_TESTS "Build separately admitted official Redshift import comparison tests" OFF)
option(ODBCPP_CRYPTO_PROFILE_LIVE_TESTS "Build mandatory real-driver TLS profile tests" OFF)
option(ODBCPP_CRYPTO_TLS_PROOF "Run the independent native TLS qualification peer" OFF)
# The dm_it_* targets always build. Connected suites remain opt-in because they
# need a configured database; the bounded dm-offline subset always registers.
option(ODBCPP_DM_INTEGRATION_TESTS "Register the integration suites built against the Driver Manager" OFF)
# Offline graph check: an opt-in registration must not name an image that the
# ordinary build skips, or a full collection reports it "Not Run".
add_test(NAME test_registered_test_images COMMAND ${CMAKE_COMMAND}
  -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
  -DBUILD_DIR=${CMAKE_BINARY_DIR}
  -DCONFIG=$<CONFIG>
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckRegisteredTestImages.cmake)
set_tests_properties(test_registered_test_images PROPERTIES LABELS "unit;architecture" TIMEOUT 60)
add_executable(crypto_identity_probe tests/security/crypto_identity_probe.cpp)
target_link_libraries(crypto_identity_probe PRIVATE ${PROJECT_NAME}::core)
apply_compiler_settings(crypto_identity_probe)
set_target_properties(crypto_identity_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME test_crypto_runtime_identity COMMAND ${CMAKE_COMMAND}
  -DPROBE=$<TARGET_FILE:crypto_identity_probe>
  -DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-manifest.json
  -DARTIFACT=$<TARGET_FILE:${PROJECT_NAME}_driver>
  -DEVIDENCE=${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-identity-evidence.json
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RecordCryptoIdentity.cmake)
add_test(NAME test_crypto_identity_evidence_rules COMMAND ${CMAKE_COMMAND}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestCryptoIdentityEvidence.cmake)
add_test(NAME test_crypto_identity_evidence_recording COMMAND ${CMAKE_COMMAND}
  -DPROBE=$<TARGET_FILE:crypto_identity_probe>
  -DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-manifest.json
  -DARTIFACT=$<TARGET_FILE:${PROJECT_NAME}_driver>
  -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestCryptoIdentityRecording.cmake)
set_tests_properties(test_crypto_runtime_identity test_crypto_identity_evidence_rules
  test_crypto_identity_evidence_recording
  PROPERTIES LABELS "unit;security" TIMEOUT 30)
if(WIN32 AND ODBCPP_CRYPTO_LINKAGE STREQUAL "BUNDLED_SHARED")
  # Build-tree probes have no packaged DLL siblings. Select their configured
  # runtime prefix explicitly rather than whichever OpenSSL another tool puts
  # first on the host PATH. Packaged-driver loader tests remain independent.
  set_tests_properties(test_crypto_runtime_identity test_crypto_identity_evidence_recording
    PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${ODBCPP_CRYPTO_ROOT_CANONICAL}/bin")
endif()
if(ODBCPP_CRYPTO_TLS_PROOF)
  if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin|Windows)$")
    message(FATAL_ERROR "The independent TLS qualification peer requires Linux, macOS or Windows")
  endif()
  find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
  find_program(ODBCPP_TLS_PEER_OPENSSL openssl REQUIRED)
  add_executable(crypto_tls_probe tests/security/tls_probe.cpp)
  target_link_libraries(crypto_tls_probe PRIVATE ${PROJECT_NAME}::core)
  apply_compiler_settings(crypto_tls_probe)
  set_target_properties(crypto_tls_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  add_test(NAME verified_crypto_tls_interop COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/security/test_verified_tls.py"
    --probe $<TARGET_FILE:crypto_tls_probe> --openssl "${ODBCPP_TLS_PEER_OPENSSL}")
  set_tests_properties(verified_crypto_tls_interop PROPERTIES LABELS "unit;security" TIMEOUT 90)
  if(WIN32 AND ODBCPP_CRYPTO_LINKAGE STREQUAL "BUNDLED_SHARED")
    set_tests_properties(verified_crypto_tls_interop PROPERTIES ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:${ODBCPP_CRYPTO_ROOT_CANONICAL}/bin")
  endif()
endif()

# Private live MySQL connection proof; invoked explicitly by the pinned fixture.
add_executable(mysql_auth_probe tests/security/mysql_auth_probe.cpp)
target_link_libraries(mysql_auth_probe PRIVATE ${PROJECT_NAME}::core)
apply_compiler_settings(mysql_auth_probe)
set_target_properties(mysql_auth_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Pinned-fixture MySQL executables; the fixture runs them, default CTest never does.
option(ODBCPP_MYSQL_DECIMAL_LIVE_TESTS "Build pinned MySQL decimal result qualification" OFF)
option(ODBCPP_MYSQL_DATETIME_LIVE_TESTS "Build pinned MySQL valid DATETIME result qualification" OFF)
option(ODBCPP_MYSQL_DATE_PARAMETERS_LIVE_TESTS "Build pinned MySQL valid DATE parameter qualification" OFF)
option(ODBCPP_MYSQL_DATETIME_RECEIPT_LIVE_TESTS "Build pinned MySQL DATETIME prepare metadata observation" OFF)
option(ODBCPP_MYSQL_DECIMAL_RECEIPT_LIVE_TESTS "Build pinned MySQL DECIMAL prepare metadata observation" OFF)
set(ODBCPP_MYSQL_FIXTURE_TESTS
  DECIMAL it_mysql_decimal_results
  DATETIME it_mysql_datetime_results
  DATE_PARAMETERS it_mysql_date_parameters
  DATETIME_RECEIPT it_mysql_datetime_parameter_receipt
  DECIMAL_RECEIPT it_mysql_decimal_parameter_receipt)
set(_mysql_fixture_test_names "")
while(ODBCPP_MYSQL_FIXTURE_TESTS)
  list(POP_FRONT ODBCPP_MYSQL_FIXTURE_TESTS _option _test)
  list(APPEND _mysql_fixture_test_names ${_test})
  if(ODBCPP_MYSQL_${_option}_LIVE_TESTS)
    add_executable(${_test} tests/integration/${_test}.cpp)
    target_link_libraries(${_test} PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
    apply_compiler_settings(${_test})
    set_target_properties(${_test} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  endif()
endwhile()

# ---- Unit Tests ----
# Optional SDK consumers and dynamic-load tests are explicitly registered below.
set(_unit_tests_registered_elsewhere
  test_named_profile_acquisition test_provisioned_native_owner
  test_provisioned_pg_connector test_provisioned_sdk_runtime
  test_sts_serverless_owner test_jwt_iam_auth test_idc_token_owner
  test_browser_idc_owner test_odbc_trace_reload)
file(GLOB UNIT_TEST_SOURCES "tests/unit/*.cpp")
foreach(test_file ${UNIT_TEST_SOURCES})
  get_filename_component(test_name ${test_file} NAME_WE)
  if(test_name IN_LIST _unit_tests_registered_elsewhere)
    continue()
  endif()
  add_test_executable(${test_name} ${test_file})
  set_tests_properties(${test_name} PROPERTIES LABELS "unit")
endforeach()
target_include_directories(test_pg_credential_consumer PRIVATE "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}"
  "${ODBCPP_INTERNAL_INCLUDE_auth}" "${ODBCPP_INTERNAL_INCLUDE_backend}")
foreach(_test IN LISTS ODBCPP_AUTH_CORE_UNIT_TESTS)
  target_include_directories(${_test} PRIVATE "${ODBCPP_INTERNAL_INCLUDE_auth}")
endforeach()
target_link_libraries(test_connection_pool PRIVATE odbcpp_prototype_pool)
target_link_libraries(test_thread_safety PRIVATE odbcpp_prototype_pool)
target_link_libraries(test_response_operation PRIVATE Threads::Threads)
target_link_libraries(test_session_cancellation PRIVATE Threads::Threads)
target_sources(test_pg_staged_refusal_observer PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/pg_staged_refusal_observer.cpp")
target_compile_definitions(test_odbc_trace PRIVATE
  ODBCPP_EXPORTS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/odbc/odbcpp.exports")
# Loads the real driver library like a Driver Manager; it must not link the
# static core, so unload runs the driver image's own shutdown path.
add_executable(test_odbc_trace_reload tests/unit/test_odbc_trace_reload.cpp)
target_include_directories(test_odbc_trace_reload PRIVATE ${ODBC_INCLUDE_DIR})
target_link_libraries(test_odbc_trace_reload PRIVATE GTest::gtest ${CMAKE_DL_LIBS})
target_compile_definitions(test_odbc_trace_reload PRIVATE
  ODBCPP_DRIVER_LIBRARY_PATH="$<TARGET_FILE:${PROJECT_NAME}_driver>")
apply_compiler_settings(test_odbc_trace_reload)
add_dependencies(test_odbc_trace_reload ${PROJECT_NAME}_driver)
set_target_properties(test_odbc_trace_reload PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME test_odbc_trace_reload COMMAND test_odbc_trace_reload)
set_tests_properties(test_odbc_trace_reload PROPERTIES LABELS "unit" TIMEOUT 60
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" SKIP_RETURN_CODE 77)
# The same native main with isolated synthetic assertions proves failure/skip
# precedence through actual GoogleTest and CTest, without loading the driver.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_xml_evidence COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_xml_evidence.py")
set_tests_properties(test_xml_evidence PROPERTIES LABELS "unit;security" TIMEOUT 60)
add_test(NAME test_aws_sdk_provenance COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/development/dependencies/test_aws_sdk_provenance.py")
set_tests_properties(test_aws_sdk_provenance PROPERTIES LABELS "unit;security" TIMEOUT 60)
# This lifecycle runs on the POSIX coordinator (including WSL), not native Windows.
if(UNIX)
  add_test(NAME test_redshift_ca_snapshot COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/development/test_redshift_ca_snapshot.py")
  set_tests_properties(test_redshift_ca_snapshot PROPERTIES LABELS "unit;security" TIMEOUT 60)
endif()
# Compile the dependency regressions against the exact patched bundled headers.
add_executable(test_bundled_fmt_security tools/ci/test_bundled_fmt_security.cpp)
target_include_directories(test_bundled_fmt_security PRIVATE "${SPDLOG_INCLUDE_DIR}")
target_compile_definitions(test_bundled_fmt_security PRIVATE FMT_HEADER_ONLY=1)
target_link_libraries(test_bundled_fmt_security PRIVATE GTest::gtest_main)
apply_compiler_settings(test_bundled_fmt_security)
set_target_properties(test_bundled_fmt_security PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME test_bundled_fmt_security COMMAND test_bundled_fmt_security)
set_tests_properties(test_bundled_fmt_security PROPERTIES LABELS "unit;security" TIMEOUT 30)
add_test(NAME test_spdlog_fmt_patch COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_spdlog_fmt_patch.py"
  --cmake "${CMAKE_COMMAND}" --spdlog-source "${spdlog_SOURCE_DIR}")
set_tests_properties(test_spdlog_fmt_patch PROPERTIES LABELS "unit;security" TIMEOUT 120)
add_executable(trace_reload_exit_policy_control tests/unit/test_odbc_trace_reload.cpp)
target_include_directories(trace_reload_exit_policy_control PRIVATE ${ODBC_INCLUDE_DIR})
target_compile_definitions(trace_reload_exit_policy_control PRIVATE ODBCPP_TRACE_RELOAD_EXIT_POLICY_CONTROL=1)
target_link_libraries(trace_reload_exit_policy_control PRIVATE GTest::gtest)
apply_compiler_settings(trace_reload_exit_policy_control)
set_target_properties(trace_reload_exit_policy_control PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME test_trace_reload_exit_policy COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/security/test_trace_reload_exit_policy.py"
  --binary $<TARGET_FILE:trace_reload_exit_policy_control>
  --ctest "${CMAKE_CTEST_COMMAND}" --work-root "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(test_trace_reload_exit_policy PROPERTIES LABELS "unit;architecture" TIMEOUT 120)
# The backend contract suite exceeds the standard COFF section limit on MSVC.
if(MSVC)
  target_compile_options(test_backend_contract PRIVATE /bigobj)
  # Preserve module-relative fault locations without changing optimization.
  target_link_options(test_backend_contract PRIVATE
    "/MAP:$<TARGET_FILE_DIR:test_backend_contract>/test_backend_contract.map")
endif()
set_tests_properties(test_pg_credential_consumer ${ODBCPP_AUTH_CORE_UNIT_TESTS}
  PROPERTIES LABELS "unit;security" TIMEOUT 30)
set_tests_properties(test_mysql_handshake_wire test_mysql_connection_security test_mysql_tls_negotiation test_mysql_authentication test_mysql_session test_mysql_query_wire test_mysql_prepared_wire test_mysql_connect_until test_mysql_datetime_q6_preflight_deadlines test_mysql_q6_preflight_phase_binding
  PROPERTIES LABELS "unit;security" TIMEOUT 30)

if((CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin") AND
   ODBCPP_CRYPTO_LINKAGE STREQUAL "SYSTEM_SHARED")
  add_executable(test_shared_crypto_cohabitation
    tests/security/test_shared_crypto_cohabitation.cpp)
  target_include_directories(test_shared_crypto_cohabitation PRIVATE
    ${ODBC_INCLUDE_DIR} ${OPENSSL_INCLUDE_DIR})
  target_link_libraries(test_shared_crypto_cohabitation PRIVATE GTest::gtest_main ${CMAKE_DL_LIBS})
  target_compile_definitions(test_shared_crypto_cohabitation PRIVATE
    ODBCPP_COHABITATION_SSL_LIBRARY="${OPENSSL_SSL_LIBRARY}"
    ODBCPP_COHABITATION_CRYPTO_LIBRARY="${OPENSSL_CRYPTO_LIBRARY}"
    ODBCPP_COHABITATION_VERSION="${OPENSSL_VERSION}"
    ODBCPP_DRIVER_LIBRARY_PATH="$<TARGET_FILE:${PROJECT_NAME}_driver>")
  apply_compiler_settings(test_shared_crypto_cohabitation)
  add_dependencies(test_shared_crypto_cohabitation ${PROJECT_NAME}_driver)
  add_test(NAME test_shared_crypto_cohabitation COMMAND test_shared_crypto_cohabitation)
  set_tests_properties(test_shared_crypto_cohabitation PROPERTIES LABELS "unit;architecture;security" TIMEOUT 30)
  set_target_properties(test_shared_crypto_cohabitation PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   ODBCPP_CRYPTO_LINKAGE STREQUAL "BUNDLED_STATIC" AND
   ((ODBCPP_CRYPTO_COHABITATION_SSL_LIBRARY AND
     NOT ODBCPP_CRYPTO_COHABITATION_CRYPTO_LIBRARY) OR
    (ODBCPP_CRYPTO_COHABITATION_CRYPTO_LIBRARY AND
     NOT ODBCPP_CRYPTO_COHABITATION_SSL_LIBRARY)))
  message(FATAL_ERROR
    "Both crypto cohabitation libraries must be provided together")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   ODBCPP_CRYPTO_LINKAGE STREQUAL "BUNDLED_STATIC" AND
   ODBCPP_CRYPTO_COHABITATION_SSL_LIBRARY AND
   ODBCPP_CRYPTO_COHABITATION_CRYPTO_LIBRARY)
  foreach(_cohabitation_library IN ITEMS
      "${ODBCPP_CRYPTO_COHABITATION_SSL_LIBRARY}"
      "${ODBCPP_CRYPTO_COHABITATION_CRYPTO_LIBRARY}")
    if(NOT IS_ABSOLUTE "${_cohabitation_library}" OR
       NOT EXISTS "${_cohabitation_library}")
      message(FATAL_ERROR
        "Crypto cohabitation libraries must be existing absolute files")
    endif()
  endforeach()
  add_executable(test_static_crypto_cohabitation
    tests/security/test_static_crypto_cohabitation.cpp)
  target_include_directories(test_static_crypto_cohabitation PRIVATE
    ${ODBC_INCLUDE_DIR})
  target_link_libraries(test_static_crypto_cohabitation PRIVATE
    GTest::gtest_main ${CMAKE_DL_LIBS})
  target_compile_definitions(test_static_crypto_cohabitation PRIVATE
    ODBCPP_COHABITATION_SSL_LIBRARY="${ODBCPP_CRYPTO_COHABITATION_SSL_LIBRARY}"
    ODBCPP_COHABITATION_CRYPTO_LIBRARY="${ODBCPP_CRYPTO_COHABITATION_CRYPTO_LIBRARY}"
    ODBCPP_DRIVER_LIBRARY_PATH="$<TARGET_FILE:${PROJECT_NAME}_driver>")
  apply_compiler_settings(test_static_crypto_cohabitation)
  add_dependencies(test_static_crypto_cohabitation ${PROJECT_NAME}_driver)
  add_test(NAME test_static_crypto_cohabitation
    COMMAND test_static_crypto_cohabitation)
  set_tests_properties(test_static_crypto_cohabitation
    PROPERTIES LABELS "unit;architecture")
  set_target_properties(test_static_crypto_cohabitation PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

add_test(
  NAME test_architecture_boundaries
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckArchitecture.cmake)
set_tests_properties(test_architecture_boundaries PROPERTIES LABELS "unit;architecture")

add_test(
  NAME test_unimplemented_backend_rejected
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/unsupported-backend-check
          -DCRYPTO_PROVIDER=${ODBCPP_CRYPTO_PROVIDER}
          -DCRYPTO_LINKAGE=${ODBCPP_CRYPTO_LINKAGE}
          -DCRYPTO_ROOT=${ODBCPP_CRYPTO_ROOT}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestUnsupportedBackend.cmake)
set_tests_properties(test_unimplemented_backend_rejected
  PROPERTIES LABELS "unit;architecture")

add_test(
  NAME test_crypto_profile_validation
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestCryptoProfileValidation.cmake)
set_tests_properties(test_crypto_profile_validation
  PROPERTIES LABELS "unit;architecture")

add_test(
  NAME test_native_aws_sdk_import
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestNativeAwsSdkImport.cmake)
set_tests_properties(test_native_aws_sdk_import
  PROPERTIES LABELS "unit;architecture" TIMEOUT 120)

add_test(
  NAME test_crypto_manifest
  COMMAND ${CMAKE_COMMAND}
          -DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-manifest.json
          -DEXPECTED_PROVIDER=${ODBCPP_CRYPTO_PROVIDER}
          -DEXPECTED_LINKAGE=${ODBCPP_CRYPTO_LINKAGE}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCryptoManifest.cmake)
set_tests_properties(test_crypto_manifest PROPERTIES LABELS "unit;architecture")

if(WIN32)
  add_test(
    NAME test_crypto_inspector_without_tests
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            "-DLINKER=${CMAKE_LINKER}"
            "-DGENERATOR=${CMAKE_GENERATOR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestCryptoInspectorConfiguration.cmake)
  set_tests_properties(test_crypto_inspector_without_tests
    PROPERTIES LABELS "unit;architecture")
elseif(APPLE)
  find_program(ODBCPP_CRYPTO_INSPECTOR otool)
elseif(UNIX)
  find_program(ODBCPP_CRYPTO_INSPECTOR readelf)
  find_program(ODBCPP_CRYPTO_RESOLVER ldd)
endif()
add_test(
  NAME test_crypto_artifact_dependency_form
  COMMAND ${CMAKE_COMMAND}
          -DARTIFACT=$<TARGET_FILE:${PROJECT_NAME}_driver>
          -DEXPECTED_PROVIDER=${ODBCPP_CRYPTO_PROVIDER}
          -DEXPECTED_LINKAGE=${ODBCPP_CRYPTO_LINKAGE}
          -DPLATFORM=${CMAKE_SYSTEM_NAME}
          -DINSPECTOR=${ODBCPP_CRYPTO_INSPECTOR}
          -DRESOLVER=${ODBCPP_CRYPTO_RESOLVER}
          -DCONFIG_MANIFEST=${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-manifest.json
          -DSTATIC_LINK_MAP=${ODBCPP_CRYPTO_STATIC_LINK_MAP}
          -DSTATIC_LINKER_IDENTITY=${ODBCPP_CRYPTO_STATIC_LINKER_IDENTITY}
          -DEVIDENCE=${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-artifact-evidence.json
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/InspectCryptoArtifact.cmake)
set_tests_properties(test_crypto_artifact_dependency_form
  PROPERTIES LABELS "unit;architecture")

add_test(
  NAME test_crypto_artifact_inspection_rules
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestCryptoArtifactInspection.cmake)
set_tests_properties(test_crypto_artifact_inspection_rules
  PROPERTIES LABELS "unit;architecture")

if(WIN32)
  set(ODBCPP_DRIVER_EXPORT_INSPECTOR "${ODBCPP_CRYPTO_INSPECTOR}")
else()
  find_program(ODBCPP_DRIVER_EXPORT_INSPECTOR nm REQUIRED)
endif()
add_test(
  NAME test_driver_export_surface
  COMMAND ${CMAKE_COMMAND}
          -DARTIFACT=$<TARGET_FILE:${PROJECT_NAME}_driver>
          -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/odbc/odbcpp.exports
          -DPLATFORM=${CMAKE_SYSTEM_NAME}
          -DINSPECTOR=${ODBCPP_DRIVER_EXPORT_INSPECTOR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckDriverExports.cmake)
set_tests_properties(test_driver_export_surface
  PROPERTIES LABELS "unit;architecture")

add_test(
  NAME test_driver_export_rules
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestDriverExports.cmake)
set_tests_properties(test_driver_export_rules
  PROPERTIES LABELS "unit;architecture")

add_test(
  NAME test_driver_export_inventory
  COMMAND ${CMAKE_COMMAND}
          -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/odbc/odbcpp.exports
          -DINVENTORY=${CMAKE_CURRENT_SOURCE_DIR}/tests/test_driver_exports.h
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckDriverExportInventory.cmake)
set_tests_properties(test_driver_export_inventory
  PROPERTIES LABELS "unit;architecture")

if(TARGET test_driver_capabilities)
  add_dependencies(test_driver_capabilities odbcpp_driver)
  target_compile_definitions(test_driver_capabilities PRIVATE
    ODBCPP_DRIVER_LIBRARY_PATH="$<TARGET_FILE:odbcpp_driver>")
  if(CMAKE_DL_LIBS)
    target_link_libraries(test_driver_capabilities PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

option(ODBCPP_NATIVE_PROVISIONED_AUTH_TESTS "Build the separately admitted native provisioned auth test" OFF)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NativeProvisionedAuth.cmake")

option(ODBCPP_REDSHIFT_NATIVE_IDC_TOKEN_TESTS "Build the native IdC token owner (offline fake-HTTP + separately admitted live login)" OFF)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NativeIdcTokenAuth.cmake")

# ---- Integration Tests ----
# Authored real Redshift routine A/W coverage is opt-in execution only. Keep a
# separately buildable target for compile checks without placing it in ordinary
# local regression or the PostgreSQL fixture graph.
option(ODBCPP_REDSHIFT_ROUTINE_SCHEMA_LIVE_TESTS "Run separately admitted real Redshift routine schema tests" OFF)
if(TARGET_DATABASE STREQUAL "REDSHIFT")
  add_executable(it_redshift_routine_schema_patterns EXCLUDE_FROM_ALL
    tests/integration/it_redshift_routine_schema_patterns.cpp)
  target_link_libraries(it_redshift_routine_schema_patterns PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
  apply_compiler_settings(it_redshift_routine_schema_patterns)
  set_target_properties(it_redshift_routine_schema_patterns PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  if(ODBCPP_REDSHIFT_ROUTINE_SCHEMA_LIVE_TESTS)
    # A registered test must be built by the ordinary graph, not only on request.
    set_target_properties(it_redshift_routine_schema_patterns PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME it_redshift_routine_schema_patterns COMMAND it_redshift_routine_schema_patterns)
    set_tests_properties(it_redshift_routine_schema_patterns PROPERTIES
      LABELS "integration;redshift;routine" TIMEOUT 120)
  endif()
endif()
# SQLColumns omitted-table native coverage: compile/link is ordinary evidence;
# execution remains separately admitted and absent from the default CTest graph.
option(ODBCPP_REDSHIFT_COLUMNS_OMITTED_TABLE_LIVE_TESTS "Run separately admitted Redshift schema-wide SQLColumns tests" OFF)
if(TARGET_DATABASE STREQUAL "REDSHIFT")
  add_executable(it_redshift_columns_omitted_table EXCLUDE_FROM_ALL
    tests/integration/it_redshift_columns_omitted_table.cpp)
  target_link_libraries(it_redshift_columns_omitted_table PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
  apply_compiler_settings(it_redshift_columns_omitted_table)
  set_target_properties(it_redshift_columns_omitted_table PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  if(ODBCPP_REDSHIFT_COLUMNS_OMITTED_TABLE_LIVE_TESTS)
    set_target_properties(it_redshift_columns_omitted_table PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME it_redshift_columns_omitted_table COMMAND it_redshift_columns_omitted_table)
    set_tests_properties(it_redshift_columns_omitted_table PROPERTIES LABELS "integration;redshift;columns" TIMEOUT 120)
  endif()
endif()
# Native IdP token login (direct-token) live test: opt-in, separately admitted
# against a real Redshift + Identity Center token. Compile/link is ordinary
# evidence; execution is absent from the default CTest graph.
option(ODBCPP_REDSHIFT_NATIVE_IDP_LIVE_TESTS "Run separately admitted Redshift native IdP direct-token live tests" OFF)
if(TARGET_DATABASE STREQUAL "REDSHIFT")
  add_executable(it_redshift_native_idp_live EXCLUDE_FROM_ALL
    tests/integration/it_redshift_native_idp_live.cpp)
  target_link_libraries(it_redshift_native_idp_live PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
  apply_compiler_settings(it_redshift_native_idp_live)
  set_target_properties(it_redshift_native_idp_live PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  if(ODBCPP_REDSHIFT_NATIVE_IDP_LIVE_TESTS)
    add_test(NAME it_redshift_native_idp_live COMMAND it_redshift_native_idp_live)
    set_tests_properties(it_redshift_native_idp_live PROPERTIES
      LABELS "integration;redshift;native-idp;live" TIMEOUT 120)
  endif()
endif()
# Opt-in native, auth and pinned-fixture images are registered explicitly; the
# Driver Manager suite never links odbcpp_core.
set(_integration_tests_registered_elsewhere
  it_redshift_routine_schema_patterns it_redshift_columns_omitted_table
  it_redshift_powerbi_relationship it_driver_manager_unicode it_auth_pg_material_tls
  it_redshift_native_auth it_redshift_serverless_sts_auth
  it_redshift_serverless_jwt_iam_auth it_redshift_native_idp_live it_redshift_native_idc_token_live
  it_redshift_browser_idc_live   ${_mysql_fixture_test_names})
file(GLOB INTEGRATION_TEST_SOURCES "tests/integration/*.cpp")
foreach(test_file ${INTEGRATION_TEST_SOURCES})
  get_filename_component(test_name ${test_file} NAME_WE)
  if(test_name IN_LIST _integration_tests_registered_elsewhere)
    continue()
  endif()
  # This executable is release evidence for a real Redshift endpoint. Running
  # it against the PostgreSQL fixture would turn a green PostgreSQL job into a
  # false Redshift compatibility claim.
  if(test_name STREQUAL "it_redshift_real" AND
     NOT TARGET_DATABASE STREQUAL "REDSHIFT")
    continue()
  endif()
  if(test_name STREQUAL "it_redshift_native_idp" AND
     NOT TARGET_DATABASE STREQUAL "REDSHIFT")
    continue()
  endif()
  if(test_name MATCHES "^it_session_(reset|owner|baseline)$" AND NOT TARGET_DATABASE STREQUAL "POSTGRESQL")
    continue()
  endif()
  add_test_executable(${test_name} ${test_file})
  set_tests_properties(${test_name} PROPERTIES LABELS "integration")
endforeach()
target_link_libraries(it_connection_pool PRIVATE odbcpp_prototype_pool)
if(TARGET it_session_baseline)
  target_link_libraries(it_session_baseline PRIVATE odbcpp_session_baseline)
endif()

# Exercise the shared driver through the platform ODBC Driver Manager. This
# deliberately does not link odbcpp_core, so missing exports or registration
# problems cannot be hidden by the in-process integration tests.
if(UNIX OR WIN32)
  if(WIN32)
    set(_odbc_driver_manager_names odbc32)
  elseif(ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "IODBC")
    set(_odbc_driver_manager_names iodbc)
    if(NOT ODBC_DRIVER_MANAGER_INCLUDE_DIR)
      find_path(_odbc_driver_manager_include_dir sql.h
        PATHS
          /opt/homebrew/opt/libiodbc/include
          /usr/local/opt/libiodbc/include
          /usr/include/iodbc
          /usr/local/include/iodbc
          /opt/local/include/libiodbc
          /Library/Frameworks/iODBC.framework/Headers
        NO_DEFAULT_PATH)
      set(ODBC_DRIVER_MANAGER_INCLUDE_DIR
          "${_odbc_driver_manager_include_dir}")
    endif()
  elseif(ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "UNIXODBC")
    set(_odbc_driver_manager_names odbc)
    if(NOT ODBC_DRIVER_MANAGER_INCLUDE_DIR)
      set(ODBC_DRIVER_MANAGER_INCLUDE_DIR "${ODBC_INCLUDE_DIR}")
    endif()
  elseif(ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "AUTO")
    set(_odbc_driver_manager_names odbc iodbc)
    if(NOT ODBC_DRIVER_MANAGER_INCLUDE_DIR)
      set(ODBC_DRIVER_MANAGER_INCLUDE_DIR "${ODBC_INCLUDE_DIR}")
    endif()
  else()
    message(FATAL_ERROR
      "ODBC_DRIVER_MANAGER_FLAVOR must be AUTO, UNIXODBC, or IODBC")
  endif()

  if(NOT WIN32 AND NOT ODBC_DRIVER_MANAGER_INCLUDE_DIR)
    message(FATAL_ERROR
      "ODBC driver-manager headers were not found for ${ODBC_DRIVER_MANAGER_FLAVOR}")
  endif()

  if(WIN32)
    set(ODBC_DRIVER_MANAGER_LIBRARY odbc32)
  else()
    find_library(ODBC_DRIVER_MANAGER_LIBRARY NAMES ${_odbc_driver_manager_names})
  endif()
  if(ODBC_DRIVER_MANAGER_LIBRARY)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DmQualifiedIdentity.cmake")
    add_library(odbcpp_test_dm_identity STATIC tests/support/dm_identity.cpp)
    target_include_directories(odbcpp_test_dm_identity PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dm-identity")
    target_link_libraries(odbcpp_test_dm_identity PRIVATE ${CMAKE_DL_LIBS})
    apply_compiler_settings(odbcpp_test_dm_identity)
    add_executable(test_dm_identity tests/driver_manager/test_dm_identity.cpp)
    target_include_directories(test_dm_identity PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(ODBC_DRIVER_MANAGER_INCLUDE_DIR)
      target_include_directories(test_dm_identity BEFORE PRIVATE ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
    endif()
    target_compile_definitions(test_dm_identity PRIVATE ODBCPP_TEST_VIA_DRIVER_MANAGER=1)
    if(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE)
      target_compile_definitions(test_dm_identity PRIVATE
        ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DM_SQLWCHAR_SIZE})
    endif()
    if(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE)
      target_compile_definitions(test_dm_identity PRIVATE
        ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
    endif()
    target_link_libraries(test_dm_identity PRIVATE
      GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} odbcpp_test_dm_identity)
    apply_compiler_settings(test_dm_identity)
    set_target_properties(test_dm_identity PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME test_dm_identity COMMAND test_dm_identity)
    set_tests_properties(test_dm_identity PROPERTIES LABELS "unit;dm-identity" TIMEOUT 30)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Odbc2CompatibilityTests.cmake")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dm-offline")
    add_test(NAME dm_offline_identity COMMAND test_dm_identity
      "--gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/dm-offline/identity.xml")
    set_tests_properties(dm_offline_identity PROPERTIES
      LABELS "dm-offline" TIMEOUT 30
      FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\];Running 0 tests")
    # Optional relationship partition; no core link and no PostgreSQL registration.
    if(ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP_LIVE_TESTS)
      if(NOT TARGET_DATABASE STREQUAL "REDSHIFT" OR
         NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux|Windows)$" OR
         NOT ODBCPP_EXPECT_DM_SQLWCHAR_SIZE MATCHES "^(2|4)$" OR
         NOT ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE STREQUAL ODBCPP_EXPECT_DM_SQLWCHAR_SIZE)
        message(FATAL_ERROR "Relationship live tests require Redshift and an explicit matched supported DM/driver width pair")
      endif()
      if((WIN32 AND (NOT ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "AUTO" OR NOT ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "2")) OR
         (NOT WIN32 AND NOT ((ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "UNIXODBC" AND ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "2") OR
                              (ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "IODBC" AND ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "4"))))
        message(FATAL_ERROR "Relationship live tests require native Windows ODBC2, unixODBC2, or iODBC4 owning profiles")
      endif()
      add_executable(it_redshift_powerbi_relationship
        tests/integration/it_redshift_powerbi_relationship.cpp)
      if(ODBC_DRIVER_MANAGER_INCLUDE_DIR)
        target_include_directories(it_redshift_powerbi_relationship PRIVATE ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
      endif()
      target_link_libraries(it_redshift_powerbi_relationship PRIVATE
        GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} ${CMAKE_DL_LIBS})
      if(WIN32)
        target_link_libraries(it_redshift_powerbi_relationship PRIVATE advapi32)
      endif()
      target_compile_definitions(it_redshift_powerbi_relationship PRIVATE
        ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DM_SQLWCHAR_SIZE}
        ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
      apply_compiler_settings(it_redshift_powerbi_relationship)
      set_target_properties(it_redshift_powerbi_relationship PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
      add_test(NAME it_redshift_powerbi_relationship COMMAND it_redshift_powerbi_relationship)
      set_tests_properties(it_redshift_powerbi_relationship PROPERTIES
        LABELS "integration;redshift;powerbi;relationship" TIMEOUT 90)
    endif()
    add_executable(it_driver_manager tests/driver_manager/it_driver_manager.cpp)
    if(ODBC_DRIVER_MANAGER_INCLUDE_DIR)
      target_include_directories(it_driver_manager PRIVATE
        ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
    endif()
    target_link_libraries(it_driver_manager PRIVATE ${ODBC_DRIVER_MANAGER_LIBRARY})
    if(ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "IODBC")
      target_compile_definitions(it_driver_manager PRIVATE
        ODBCPP_TEST_IODBC=1)
    endif()
    if(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE)
      target_compile_definitions(it_driver_manager PRIVATE
        ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DM_SQLWCHAR_SIZE})
    endif()
    if(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE)
      target_compile_definitions(it_driver_manager PRIVATE
        ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
    endif()

    # This consumer exercises the installed driver through the actual manager.
    # Default-off: it never joins the ordinary PostgreSQL integration graph.
    set(_redshift_package_narrow_manager FALSE)
    if((WIN32 AND ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "AUTO") OR
       (NOT WIN32 AND ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "UNIXODBC"))
      set(_redshift_package_narrow_manager TRUE)
    endif()
    if(ODBCPP_REDSHIFT_PACKAGE_LIVE_TESTS)
      if(NOT TARGET_DATABASE STREQUAL "REDSHIFT" OR
         NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux|Windows)$" OR
         NOT _redshift_package_narrow_manager OR
         NOT ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "2" OR
         NOT ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE STREQUAL "2")
        message(FATAL_ERROR
          "Installed Redshift live tests require a supported OS, Windows ODBC or unixODBC, and explicit matched SQLWCHAR size 2")
      endif()
      add_executable(it_redshift_package_live
        tests/driver_manager/it_redshift_package_live.cpp)
      target_include_directories(it_redshift_package_live PRIVATE
        ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
      target_link_libraries(it_redshift_package_live PRIVATE
        GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} ${CMAKE_DL_LIBS})
      if(WIN32)
        target_link_libraries(it_redshift_package_live PRIVATE advapi32)
      endif()
      target_compile_definitions(it_redshift_package_live PRIVATE
        ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=2
        ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=2)
      apply_compiler_settings(it_redshift_package_live)
      add_test(NAME it_redshift_package_live COMMAND it_redshift_package_live)
      set_tests_properties(it_redshift_package_live PROPERTIES LABELS "integration;redshift;package")
      set_target_properties(it_redshift_package_live PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    endif()

    if(ODBCPP_REDSHIFT_PACKAGE_IMPORT_LIVE_TESTS)
      # Bound buffers require matching application and driver wide-code units.
      # These explicit pairs are package candidates, not an Excel ABI observation.
      set(_redshift_import_matched_pair FALSE)
      if((_redshift_package_narrow_manager AND
          ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "2" AND
          ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE STREQUAL "2") OR
         (NOT WIN32 AND ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "IODBC" AND
          ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "4" AND
          ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE STREQUAL "4"))
        set(_redshift_import_matched_pair TRUE)
      endif()
      if(NOT TARGET_DATABASE STREQUAL "REDSHIFT" OR
         NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux|Windows)$" OR
         NOT _redshift_import_matched_pair)
        message(FATAL_ERROR
          "Installed Redshift import live tests require a supported OS and explicit Windows ODBC/unixODBC 2/2 or iODBC 4/4")
      endif()
      add_executable(it_redshift_package_import_live
        tests/driver_manager/it_redshift_package_import_live.cpp)
      target_include_directories(it_redshift_package_import_live PRIVATE
        ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
      target_link_libraries(it_redshift_package_import_live PRIVATE
        GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} ${CMAKE_DL_LIBS})
      if(WIN32)
        target_link_libraries(it_redshift_package_import_live PRIVATE advapi32)
      endif()
      target_compile_definitions(it_redshift_package_import_live PRIVATE
        ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DM_SQLWCHAR_SIZE}
        ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
      apply_compiler_settings(it_redshift_package_import_live)
      add_test(NAME it_redshift_package_import_live COMMAND it_redshift_package_import_live)
      set_tests_properties(it_redshift_package_import_live PROPERTIES LABELS "integration;redshift;package")
      set_target_properties(it_redshift_package_import_live PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    endif()

    if(ODBCPP_REDSHIFT_OFFICIAL_IMPORT_LIVE_TESTS)
      if(NOT TARGET_DATABASE STREQUAL "REDSHIFT" OR
         NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
         NOT ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "IODBC" OR
         NOT ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "4" OR
         NOT ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE STREQUAL "4")
        message(FATAL_ERROR
          "Official Redshift import comparison requires Darwin and explicit IODBC 4/4")
      endif()
      add_executable(it_redshift_official_import_live
        tests/driver_manager/it_redshift_package_import_live.cpp)
      target_include_directories(it_redshift_official_import_live PRIVATE
        ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
      target_link_libraries(it_redshift_official_import_live PRIVATE
        GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} ${CMAKE_DL_LIBS})
      target_compile_definitions(it_redshift_official_import_live PRIVATE
        ODBCPP_OFFICIAL_REDSHIFT_PROFILE=1
        ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=4
        ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=4)
      apply_compiler_settings(it_redshift_official_import_live)
      add_test(NAME it_redshift_official_import_live COMMAND it_redshift_official_import_live)
      set_tests_properties(it_redshift_official_import_live PROPERTIES
        LABELS "integration;redshift;package;official")
      set_target_properties(it_redshift_official_import_live PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    endif()

    if(ODBCPP_CRYPTO_PROFILE_LIVE_TESTS OR
       (CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$" AND
        (ODBCPP_CRYPTO_TLS_PROOF OR
         (ODBCPP_CRYPTO_LINKAGE STREQUAL "BUNDLED_STATIC" AND
          ODBCPP_CRYPTO_COHABITATION_SSL_LIBRARY AND
          ODBCPP_CRYPTO_COHABITATION_CRYPTO_LIBRARY))))
      add_executable(it_crypto_profile_live
        tests/driver_manager/it_crypto_profile_live.cpp)
      target_include_directories(it_crypto_profile_live PRIVATE
        ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
      target_link_libraries(it_crypto_profile_live PRIVATE
        GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} ${CMAKE_DL_LIBS})
      target_compile_definitions(it_crypto_profile_live PRIVATE
        ODBCPP_DRIVER_LIBRARY_PATH="$<TARGET_FILE:${PROJECT_NAME}_driver>")
      apply_compiler_settings(it_crypto_profile_live)
      add_dependencies(it_crypto_profile_live ${PROJECT_NAME}_driver)
      add_test(NAME it_crypto_profile_live COMMAND it_crypto_profile_live)
      set_tests_properties(it_crypto_profile_live
        PROPERTIES LABELS "integration")
      set_target_properties(it_crypto_profile_live PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
      if(UNIX AND TARGET_DATABASE STREQUAL "POSTGRESQL")
        add_executable(it_auth_pg_material_tls tests/integration/it_auth_pg_material_tls.cpp)
        target_link_libraries(it_auth_pg_material_tls PRIVATE odbcpp_auth_pg_consumer GTest::gtest_main)
        target_include_directories(it_auth_pg_material_tls PRIVATE
          "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}" "${ODBCPP_INTERNAL_INCLUDE_auth}"
          "${ODBCPP_INTERNAL_INCLUDE_backend}" "${ODBCPP_INTERNAL_INCLUDE_runtime}")
        apply_compiler_settings(it_auth_pg_material_tls)
        add_test(NAME it_auth_pg_material_tls COMMAND it_auth_pg_material_tls)
        set_tests_properties(it_auth_pg_material_tls PROPERTIES LABELS "integration;security" TIMEOUT 30)
        set_target_properties(it_auth_pg_material_tls PROPERTIES
          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
      endif()
    endif()

    apply_compiler_settings(it_driver_manager)
    add_test(NAME it_driver_manager COMMAND it_driver_manager)
    # A plain executable: it writes no GoogleTest report.
    set_tests_properties(it_driver_manager PROPERTIES LABELS "integration;no-gtest-report")
    set_target_properties(it_driver_manager PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # Unicode metadata/truncation/diagnostic contracts as the application sees
    # them through the real manager, against the same registered driver.
    add_executable(it_driver_manager_unicode tests/integration/it_driver_manager_unicode.cpp)
    if(ODBC_DRIVER_MANAGER_INCLUDE_DIR)
      target_include_directories(it_driver_manager_unicode PRIVATE
        ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
    endif()
    target_link_libraries(it_driver_manager_unicode PRIVATE
      GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY})
    if(ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "IODBC")
      target_compile_definitions(it_driver_manager_unicode PRIVATE ODBCPP_TEST_IODBC=1)
    endif()
    if(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE)
      target_compile_definitions(it_driver_manager_unicode PRIVATE
        ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DM_SQLWCHAR_SIZE})
    endif()
    if(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE)
      target_compile_definitions(it_driver_manager_unicode PRIVATE
        ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
    endif()
    apply_compiler_settings(it_driver_manager_unicode)
    add_test(NAME it_driver_manager_unicode COMMAND it_driver_manager_unicode)
    set_tests_properties(it_driver_manager_unicode PROPERTIES LABELS "integration" TIMEOUT 90)
    set_target_properties(it_driver_manager_unicode PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # The same integration sources built against the manager and the
    # registered driver instead of odbcpp_core. The direct targets above stay
    # the diagnostic baseline; cases that need driver internals are compiled
    # out by ODBCPP_TEST_VIA_DRIVER_MANAGER. See docs/plans/driver-manager-test-layer.md.
    if(TARGET_DATABASE STREQUAL "REDSHIFT")
      set(_dm_redshift_product 1)
    else()
      set(_dm_redshift_product 0)
    endif()
    set(_dm_suites it_prepared_statements_real it_bind_col_real
        it_native_sql it_handle_lifecycle it_diagnostics it_get_info
        it_metadata_real)
    # Like its direct target, the real Redshift suite exists only in Redshift builds.
    if(TARGET_DATABASE STREQUAL "REDSHIFT")
      list(APPEND _dm_suites it_redshift_real)
    endif()
    foreach(_dm_suite IN LISTS _dm_suites)
      set(_dm_target dm_${_dm_suite})
      add_executable(${_dm_target} tests/integration/${_dm_suite}.cpp)
      if(ODBC_DRIVER_MANAGER_INCLUDE_DIR)
        target_include_directories(${_dm_target} BEFORE PRIVATE
          ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
      endif()
      target_include_directories(${_dm_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
      target_link_libraries(${_dm_target} PRIVATE
        GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} odbcpp_test_dm_identity)
      target_compile_definitions(${_dm_target} PRIVATE
        ODBCPP_TEST_VIA_DRIVER_MANAGER=1
        ODBCPP_TEST_REDSHIFT_PRODUCT=${_dm_redshift_product})
      if(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE)
        target_compile_definitions(${_dm_target} PRIVATE
          ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DM_SQLWCHAR_SIZE})
      endif()
      if(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE)
        target_compile_definitions(${_dm_target} PRIVATE
          ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
      endif()
      if(ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "IODBC")
        target_compile_definitions(${_dm_target} PRIVATE ODBCPP_TEST_IODBC=1)
      endif()
      add_dependencies(${_dm_target} ${PROJECT_NAME}_driver)
      apply_compiler_settings(${_dm_target})
      set_target_properties(${_dm_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
      if(ODBCPP_DM_INTEGRATION_TESTS)
        add_test(NAME ${_dm_target} COMMAND ${_dm_target})
        set_tests_properties(${_dm_target} PROPERTIES
          LABELS "integration;dm-integration")
      endif()
    endforeach()

    # Reuse public pre-connect API assertions on both routes. These fixtures
    # allocate/free ENV/DBC handles but never call SQLConnect/SQLDriverConnect.
    foreach(_route direct manager)
      if(_route STREQUAL "direct")
        set(_offline_target it_handle_lifecycle)
      else()
        set(_offline_target dm_it_handle_lifecycle)
      endif()
      add_test(NAME dm_offline_${_route}_login_timeout COMMAND ${_offline_target}
        --gtest_filter=ConnectionAttributeIntegrationTest.LoginTimeoutOutputWidth
        "--gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/dm-offline/${_route}-login-timeout.xml")
      set_tests_properties(dm_offline_${_route}_login_timeout PROPERTIES
        LABELS "dm-offline" TIMEOUT 30
        FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\];Running 0 tests")
    endforeach()
    # This checks the Unicode fixture conversion, not a manager API call.
    add_test(NAME dm_offline_unicode_conversion COMMAND it_driver_manager_unicode
      --gtest_filter=DriverManagerUnicodeHarnessTest.OverrideTextDecodesCompleteScalarsIntoManagerUnits
      "--gtest_output=xml:${CMAKE_CURRENT_BINARY_DIR}/dm-offline/unicode-conversion.xml")
    set_tests_properties(dm_offline_unicode_conversion PROPERTIES
      LABELS "dm-offline" TIMEOUT 30
      FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\];Running 0 tests")
    unset(_dm_redshift_product)
    unset(_dm_suites)
  elseif(NOT ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "AUTO")
    message(FATAL_ERROR
      "ODBC driver-manager library was not found for ${ODBC_DRIVER_MANAGER_FLAVOR}")
  endif()
  unset(_odbc_driver_manager_names)
  unset(_odbc_driver_manager_include_dir CACHE)
endif()

if(WIN32)
  add_test(NAME test_windows_backend_observer
    COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/test_windows_backend_observer.py")
  set_tests_properties(test_windows_backend_observer PROPERTIES LABELS "unit;windows" TIMEOUT 60)
  add_executable(test_setup_dialog tests/windows/test_setup_dialog.cpp
    odbc/setup/setup_dialog.cpp odbc/setup/setup.rc)
  target_include_directories(test_setup_dialog PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/odbc/setup")
  target_compile_definitions(test_setup_dialog PRIVATE UNICODE _UNICODE)
  target_link_libraries(test_setup_dialog PRIVATE odbccp32 odbc32 user32 comctl32
    gdi32 ole32 shell32 ${PROJECT_NAME}::core GTest::gtest_main)
  if(MSVC)
    target_link_libraries(test_setup_dialog PRIVATE legacy_stdio_definitions)
  endif()
  apply_compiler_settings(test_setup_dialog)
  set_target_properties(test_setup_dialog PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  add_test(NAME test_setup_dialog COMMAND test_setup_dialog)
  set_tests_properties(test_setup_dialog PROPERTIES LABELS "unit;windows;gui" TIMEOUT 30)

  add_executable(it_setup tests/windows/it_setup.cpp)
  target_link_libraries(it_setup PRIVATE odbccp32 user32 comctl32 gdi32)
  if(MSVC)
    target_link_libraries(it_setup PRIVATE legacy_stdio_definitions)
  endif()
  add_dependencies(it_setup odbcpp_setup odbcpp_driver)
  set_target_properties(it_setup PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  add_test(NAME it_setup COMMAND it_setup)
  # A plain executable: it writes no GoogleTest report.
  set_tests_properties(it_setup PROPERTIES LABELS "integration;no-gtest-report" TIMEOUT 90)
endif()

if(WIN32)
  # Explicit installed-package fixture; never join an unprovisioned local suite.
  option(ODBCPP_INSTALLED_SETUP_TESTS "Register tests requiring the matching installed Windows MSI" OFF)
  add_executable(it_setup_registration EXCLUDE_FROM_ALL tests/windows/it_setup_registration.cpp)
  target_link_libraries(it_setup_registration PRIVATE odbccp32 advapi32 GTest::gtest_main)
  if(MSVC)
    target_link_libraries(it_setup_registration PRIVATE legacy_stdio_definitions)
  endif()
  if(TARGET_DATABASE STREQUAL "REDSHIFT")
    target_compile_definitions(it_setup_registration PRIVATE ODBCPP_TEST_REDSHIFT_PACKAGE=1)
  endif()
  apply_compiler_settings(it_setup_registration)
  set_target_properties(it_setup_registration PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  if(ODBCPP_INSTALLED_SETUP_TESTS)
    set_target_properties(it_setup_registration PROPERTIES EXCLUDE_FROM_ALL OFF)
    add_test(NAME it_setup_registration COMMAND it_setup_registration)
    set_tests_properties(it_setup_registration PROPERTIES LABELS "integration;windows;package" TIMEOUT 30)
  endif()
  add_executable(it_package_load tests/windows/it_package_load.cpp)
  # The probe must not preload the system's C++ runtime before inspecting the DLLs.
  set_property(TARGET it_package_load PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_target_properties(it_package_load PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

add_test(NAME test_crypto_header_identity
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestCryptoIdentity.cmake)
set_tests_properties(test_crypto_header_identity PROPERTIES LABELS "unit;architecture")

add_test(NAME test_internal_source_partitions COMMAND "${CMAKE_COMMAND}"
  "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
  "-DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/internal partition fixtures"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestInternalSourcePartitions.cmake")
set_tests_properties(test_internal_source_partitions PROPERTIES LABELS "unit;architecture" TIMEOUT 30)

add_test(NAME test_internal_include_closure COMMAND "${CMAKE_COMMAND}"
  "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
  "-DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/internal include fixtures"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestInternalIncludes.cmake")
set(INCLUDE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/internal-include")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProbeInternalIncludes.cmake")
set_tests_properties(test_internal_include_closure
  PROPERTIES LABELS "unit;architecture" TIMEOUT 90)

# Backend-private first/only include; compile-only, with no source-root fallback.
add_library(odbcpp_mysql_session_header_isolation OBJECT
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/header_isolation/mysql_session_first_include.cpp")
target_compile_features(odbcpp_mysql_session_header_isolation PRIVATE cxx_std_20)
target_include_directories(odbcpp_mysql_session_header_isolation PRIVATE
  "${ODBCPP_INTERNAL_INCLUDE_backend}")
apply_compiler_settings(odbcpp_mysql_session_header_isolation)

add_test(NAME test_prototype_pool_quarantine COMMAND "${CMAKE_COMMAND}"
  "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
  "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestPrototypePool.cmake")
set_tests_properties(test_prototype_pool_quarantine PROPERTIES LABELS "unit;architecture" TIMEOUT 60)
add_test(NAME test_prototype_pool_artifacts COMMAND "${CMAKE_COMMAND}"
  "-DCORE=$<TARGET_FILE:odbcpp_core>" "-DDRIVER=$<TARGET_FILE:odbcpp_driver>"
  "-DPROTOTYPE=$<TARGET_FILE:odbcpp_prototype_pool>"
  "-DINSPECTOR=${ODBCPP_DRIVER_EXPORT_INSPECTOR}" "-DPLATFORM=${CMAKE_SYSTEM_NAME}"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPrototypeArtifacts.cmake")
set_tests_properties(test_prototype_pool_artifacts PROPERTIES LABELS "unit;architecture" TIMEOUT 60)
