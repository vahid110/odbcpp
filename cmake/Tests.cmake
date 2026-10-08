# ---- Google Test Setup ----
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---- Test Discovery and Setup ----
# MySQL connection-phase codec tests are backend-private and intentionally run
# in the protected PostgreSQL/Redshift builds before MySQL product composition.
function(add_test_executable test_name test_file)
  add_executable(${test_name} ${test_file})
  if(test_name STREQUAL "test_pg_credential_consumer")
    target_link_libraries(${test_name} PRIVATE odbcpp_auth_pg_consumer GTest::gtest_main)
    target_include_directories(${test_name} PRIVATE "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}"
      "${ODBCPP_INTERNAL_INCLUDE_auth}" "${ODBCPP_INTERNAL_INCLUDE_backend}")
  elseif(test_name MATCHES "^(test_auth_core|test_temporary_db_validity|test_issuer_timestamp|test_aws_db_response_fields|test_bounded_response_stream|test_aws_db_json_response|test_aws_db_xml_response|test_redshift_serverless_response|test_redshift_provisioned_response|test_redshift_withiam_response|test_auth_raw_response_projection|test_checked_response_boundary|test_checked_response_stream|test_checked_aws_db_json_response|test_response_operation|test_checked_aws_db_xml_response)$")
    target_link_libraries(${test_name} PRIVATE odbcpp_auth_core GTest::gtest_main)
    target_include_directories(${test_name} PRIVATE "${ODBCPP_INTERNAL_INCLUDE_auth}")
  else()
    target_link_libraries(${test_name} PRIVATE
      ${PROJECT_NAME}::core
      GTest::gtest_main
    )
  endif()
  if(test_name MATCHES "^(test_connection_pool|test_thread_safety|it_connection_pool)$")
    target_link_libraries(${test_name} PRIVATE odbcpp_prototype_pool)
  endif()
  if(test_name STREQUAL "test_pg_staged_refusal_observer")
    target_sources(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/pg_staged_refusal_observer.cpp")
  endif()
  if(test_name STREQUAL "test_response_operation")
    target_link_libraries(${test_name} PRIVATE Threads::Threads)
  endif()
  apply_compiler_settings(${test_name})
  
  add_test(NAME ${test_name} COMMAND ${test_name})
  
  # Set output directory
  set_target_properties(${test_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
  )
endfunction()

option(ODBCPP_REDSHIFT_PACKAGE_LIVE_TESTS "Build separately admitted installed Redshift Driver Manager tests" OFF)
option(ODBCPP_REDSHIFT_PACKAGE_IMPORT_LIVE_TESTS "Build separately admitted installed Redshift import/refresh tests" OFF)
option(ODBCPP_CRYPTO_PROFILE_LIVE_TESTS "Build mandatory real-driver TLS profile tests" OFF)
option(ODBCPP_CRYPTO_TLS_PROOF "Run the independent native TLS qualification peer" OFF)
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

# Explicit pinned-fixture executable; never admitted by default CTest.
option(ODBCPP_MYSQL_DECIMAL_LIVE_TESTS "Build pinned MySQL decimal result qualification" OFF)
if(ODBCPP_MYSQL_DECIMAL_LIVE_TESTS)
  add_executable(it_mysql_decimal_results tests/integration/it_mysql_decimal_results.cpp)
  target_link_libraries(it_mysql_decimal_results PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
  apply_compiler_settings(it_mysql_decimal_results)
  set_target_properties(it_mysql_decimal_results PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

option(ODBCPP_MYSQL_DATETIME_LIVE_TESTS "Build pinned MySQL valid DATETIME result qualification" OFF)
if(ODBCPP_MYSQL_DATETIME_LIVE_TESTS)
  add_executable(it_mysql_datetime_results tests/integration/it_mysql_datetime_results.cpp)
  target_link_libraries(it_mysql_datetime_results PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
  apply_compiler_settings(it_mysql_datetime_results)
  set_target_properties(it_mysql_datetime_results PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

option(ODBCPP_MYSQL_DATE_PARAMETERS_LIVE_TESTS "Build pinned MySQL valid DATE parameter qualification" OFF)
if(ODBCPP_MYSQL_DATE_PARAMETERS_LIVE_TESTS)
  add_executable(it_mysql_date_parameters tests/integration/it_mysql_date_parameters.cpp)
  target_link_libraries(it_mysql_date_parameters PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
  apply_compiler_settings(it_mysql_date_parameters)
  set_target_properties(it_mysql_date_parameters PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

# Explicit diagnostic observation only; never admitted by default CTest.
option(ODBCPP_MYSQL_DATETIME_RECEIPT_LIVE_TESTS "Build pinned MySQL DATETIME prepare metadata observation" OFF)
if(ODBCPP_MYSQL_DATETIME_RECEIPT_LIVE_TESTS)
  add_executable(it_mysql_datetime_parameter_receipt tests/integration/it_mysql_datetime_parameter_receipt.cpp)
  target_link_libraries(it_mysql_datetime_parameter_receipt PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
  apply_compiler_settings(it_mysql_datetime_parameter_receipt)
  set_target_properties(it_mysql_datetime_parameter_receipt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

# Explicit diagnostic observation only; never admitted by default CTest.
option(ODBCPP_MYSQL_DECIMAL_RECEIPT_LIVE_TESTS "Build pinned MySQL DECIMAL prepare metadata observation" OFF)
if(ODBCPP_MYSQL_DECIMAL_RECEIPT_LIVE_TESTS)
  add_executable(it_mysql_decimal_parameter_receipt tests/integration/it_mysql_decimal_parameter_receipt.cpp)
  target_link_libraries(it_mysql_decimal_parameter_receipt PRIVATE ${PROJECT_NAME}::core GTest::gtest_main)
  apply_compiler_settings(it_mysql_decimal_parameter_receipt)
  set_target_properties(it_mysql_decimal_parameter_receipt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()

# ---- Unit Tests ----
file(GLOB UNIT_TEST_SOURCES "tests/unit/*.cpp")
foreach(test_file ${UNIT_TEST_SOURCES})
  get_filename_component(test_name ${test_file} NAME_WE)
  if(test_name MATCHES "^(test_named_profile_acquisition|test_provisioned_native_owner|test_provisioned_pg_connector|test_provisioned_sdk_runtime)$")
    continue() # Optional SDK consumers are explicitly registered below.
  endif()
  add_test_executable(${test_name} ${test_file})
  set_tests_properties(${test_name} PROPERTIES LABELS "unit")
endforeach()
set_tests_properties(test_pg_credential_consumer test_auth_core test_temporary_db_validity test_issuer_timestamp test_aws_db_response_fields test_bounded_response_stream test_aws_db_json_response test_aws_db_xml_response test_redshift_serverless_response test_redshift_provisioned_response test_redshift_withiam_response test_auth_raw_response_projection test_checked_response_boundary test_checked_response_stream test_checked_aws_db_json_response test_response_operation test_checked_aws_db_xml_response
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
  NAME test_crypto_manifest
  COMMAND ${CMAKE_COMMAND}
          -DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-manifest.json
          -DEXPECTED_PROVIDER=${ODBCPP_CRYPTO_PROVIDER}
          -DEXPECTED_LINKAGE=${ODBCPP_CRYPTO_LINKAGE}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCryptoManifest.cmake)
set_tests_properties(test_crypto_manifest PROPERTIES LABELS "unit;architecture")

if(WIN32)
  get_filename_component(_linker_directory "${CMAKE_LINKER}" DIRECTORY)
  find_program(ODBCPP_CRYPTO_INSPECTOR dumpbin
    HINTS "${_linker_directory}")
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

# ---- Integration Tests ----
file(GLOB INTEGRATION_TEST_SOURCES "tests/integration/*.cpp")
foreach(test_file ${INTEGRATION_TEST_SOURCES})
  get_filename_component(test_name ${test_file} NAME_WE)
  if(test_name STREQUAL "it_auth_pg_material_tls" OR test_name STREQUAL "it_redshift_native_auth" OR test_name STREQUAL "it_mysql_decimal_results" OR test_name STREQUAL "it_mysql_datetime_results" OR test_name STREQUAL "it_mysql_date_parameters" OR test_name STREQUAL "it_mysql_datetime_parameter_receipt" OR test_name STREQUAL "it_mysql_decimal_parameter_receipt")
    continue()
  endif()
  # This executable is release evidence for a real Redshift endpoint. Running
  # it against the PostgreSQL fixture would turn a green PostgreSQL job into a
  # false Redshift compatibility claim.
  if(test_name STREQUAL "it_redshift_real" AND
     NOT TARGET_DATABASE STREQUAL "REDSHIFT")
    continue()
  endif()
  if(test_name MATCHES "^it_session_(reset|owner|baseline)$" AND NOT TARGET_DATABASE STREQUAL "POSTGRESQL")
    continue()
  endif()
  add_test_executable(${test_name} ${test_file})
  if(test_name STREQUAL "it_session_baseline")
    target_link_libraries(${test_name} PRIVATE odbcpp_session_baseline)
  endif()
  set_tests_properties(${test_name} PROPERTIES LABELS "integration")
endforeach()

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
    if(ODBCPP_REDSHIFT_PACKAGE_LIVE_TESTS)
      if(NOT TARGET_DATABASE STREQUAL "REDSHIFT" OR
         NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
         NOT ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "UNIXODBC" OR
         NOT ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "2" OR
         NOT ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE STREQUAL "2")
        message(FATAL_ERROR
          "Installed Redshift live tests require Darwin, UNIXODBC and explicit matched SQLWCHAR size 2")
      endif()
      add_executable(it_redshift_package_live
        tests/driver_manager/it_redshift_package_live.cpp)
      target_include_directories(it_redshift_package_live PRIVATE
        ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
      target_link_libraries(it_redshift_package_live PRIVATE
        GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} ${CMAKE_DL_LIBS})
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
      if((ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "UNIXODBC" AND
          ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "2" AND
          ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE STREQUAL "2") OR
         (ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "IODBC" AND
          ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "4" AND
          ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE STREQUAL "4"))
        set(_redshift_import_matched_pair TRUE)
      endif()
      if(NOT TARGET_DATABASE STREQUAL "REDSHIFT" OR
         NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
         NOT _redshift_import_matched_pair)
        message(FATAL_ERROR
          "Installed Redshift import live tests require Darwin and explicit UNIXODBC 2/2 or IODBC 4/4")
      endif()
      add_executable(it_redshift_package_import_live
        tests/driver_manager/it_redshift_package_import_live.cpp)
      target_include_directories(it_redshift_package_import_live PRIVATE
        ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
      target_link_libraries(it_redshift_package_import_live PRIVATE
        GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} ${CMAKE_DL_LIBS})
      target_compile_definitions(it_redshift_package_import_live PRIVATE
        ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DM_SQLWCHAR_SIZE}
        ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
      apply_compiler_settings(it_redshift_package_import_live)
      add_test(NAME it_redshift_package_import_live COMMAND it_redshift_package_import_live)
      set_tests_properties(it_redshift_package_import_live PROPERTIES LABELS "integration;redshift;package")
      set_target_properties(it_redshift_package_import_live PROPERTIES
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
    set_tests_properties(it_driver_manager PROPERTIES LABELS "integration")
    set_target_properties(it_driver_manager PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  elseif(NOT ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "AUTO")
    message(FATAL_ERROR
      "ODBC driver-manager library was not found for ${ODBC_DRIVER_MANAGER_FLAVOR}")
  endif()
  unset(_odbc_driver_manager_names)
  unset(_odbc_driver_manager_include_dir CACHE)
endif()

if(WIN32)
  add_executable(it_setup tests/windows/it_setup.cpp)
  target_link_libraries(it_setup PRIVATE odbccp32 user32 comctl32 gdi32)
  if(MSVC)
    target_link_libraries(it_setup PRIVATE legacy_stdio_definitions)
  endif()
  add_dependencies(it_setup odbcpp_setup odbcpp_driver)
  set_target_properties(it_setup PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  add_test(NAME it_setup COMMAND it_setup)
  set_tests_properties(it_setup PROPERTIES LABELS "integration" TIMEOUT 90)
endif()

if(WIN32)
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
