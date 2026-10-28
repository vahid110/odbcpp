# ODBC2 application tests link the actual manager, never odbcpp_core/driver.
add_executable(test_odbc2_compatibility tests/driver_manager/test_odbc2_compatibility.cpp)
target_include_directories(test_odbc2_compatibility PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(ODBC_DRIVER_MANAGER_INCLUDE_DIR)
  target_include_directories(test_odbc2_compatibility BEFORE PRIVATE ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
endif()
target_compile_definitions(test_odbc2_compatibility PRIVATE
  ODBCPP_TEST_VIA_DRIVER_MANAGER=1
  ODBCPP_ODBC2_DRIVER_PATH="$<TARGET_FILE:odbcpp_driver>")
# One observed Linux iODBC pre-driver defect, never selected by version alone.
# Unknown images retain normative assertions. Runtime compares the actual API
# provider with this configure-time snapshot before enabling the expectation.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "${ODBC_DRIVER_MANAGER_LIBRARY}")
  file(SHA256 "${ODBC_DRIVER_MANAGER_LIBRARY}" _odbc2_manager_hash)
  if(_odbc2_manager_hash STREQUAL "3451bc5bafb224754414cacda207e6693680051077515e86b5163980db31d7aa")
    file(REAL_PATH "${ODBC_DRIVER_MANAGER_LIBRARY}" _odbc2_manager_path)
    set(_odbc2_snapshot "${CMAKE_CURRENT_BINARY_DIR}/dm-identity/odbc2-iodbc.bin")
    configure_file("${_odbc2_manager_path}" "${_odbc2_snapshot}" COPYONLY)
    file(SHA256 "${_odbc2_snapshot}" _odbc2_snapshot_hash)
    if(NOT _odbc2_snapshot_hash STREQUAL _odbc2_manager_hash)
      message(FATAL_ERROR "ODBC2 manager snapshot changed while copying; qualification refused")
    endif()
    target_compile_definitions(test_odbc2_compatibility PRIVATE
      ODBCPP_ODBC2_IODBC_PATH="${_odbc2_manager_path}"
      ODBCPP_ODBC2_IODBC_SHA256="${_odbc2_manager_hash}"
      ODBCPP_ODBC2_IODBC_SNAPSHOT="${_odbc2_snapshot}")
  endif()
endif()
if(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE)
  target_compile_definitions(test_odbc2_compatibility PRIVATE
    ODBCPP_EXPECT_DM_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DM_SQLWCHAR_SIZE})
endif()
if(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE)
  target_compile_definitions(test_odbc2_compatibility PRIVATE
    ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=${ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE})
else()
  target_compile_definitions(test_odbc2_compatibility PRIVATE ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=0)
endif()
target_link_libraries(test_odbc2_compatibility PRIVATE
  GTest::gtest_main ${ODBC_DRIVER_MANAGER_LIBRARY} odbcpp_test_dm_identity ${CMAKE_DL_LIBS})
apply_compiler_settings(test_odbc2_compatibility)
set_target_properties(test_odbc2_compatibility PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
add_test(NAME dm_odbc2_predriver COMMAND test_odbc2_compatibility
  --gtest_filter=Odbc2PreDriver.*
  "--gtest_output=xml:${CMAKE_BINARY_DIR}/dm-odbc2-predriver.xml")
set_tests_properties(dm_odbc2_predriver PROPERTIES
  LABELS "unit;dm-odbc2;predriver" TIMEOUT 30
  FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\];Running 0 tests")

add_dependencies(test_odbc2_compatibility odbcpp_driver)
add_test(NAME dm_odbc2_exports COMMAND test_odbc2_compatibility
  --gtest_filter=Odbc2DriverExports.*
  "--gtest_output=xml:${CMAKE_BINARY_DIR}/dm-odbc2-exports.xml")
set_tests_properties(dm_odbc2_exports PROPERTIES
  LABELS "unit;dm-odbc2;driver-loaded" TIMEOUT 30
  FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\];Running 0 tests")

option(ODBCPP_ODBC2_CONNECTED_TESTS "Register ODBC2 tests requiring an isolated loopback PostgreSQL fixture" OFF)
if(ODBCPP_ODBC2_CONNECTED_TESTS)
  if(NOT TARGET_DATABASE STREQUAL "POSTGRESQL")
    message(FATAL_ERROR "The ODBC2 connected fixture is PostgreSQL only; Redshift is unqualified")
  endif()
  add_test(NAME dm_odbc2_connected COMMAND test_odbc2_compatibility
    --gtest_filter=EnvironmentSelection/Odbc2Connected.*
    "--gtest_output=xml:${CMAKE_BINARY_DIR}/dm-odbc2-connected.xml")
  set_tests_properties(dm_odbc2_connected PROPERTIES
    LABELS "integration;dm-odbc2;connected" TIMEOUT 120
    FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\];Running 0 tests")
endif()
