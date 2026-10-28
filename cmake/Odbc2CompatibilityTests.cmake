# ODBC2 application tests link the actual manager, never odbcpp_core/driver.
add_executable(test_odbc2_compatibility tests/driver_manager/test_odbc2_compatibility.cpp)
target_include_directories(test_odbc2_compatibility PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(ODBC_DRIVER_MANAGER_INCLUDE_DIR)
  target_include_directories(test_odbc2_compatibility BEFORE PRIVATE ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
endif()
target_compile_definitions(test_odbc2_compatibility PRIVATE
  ODBCPP_TEST_VIA_DRIVER_MANAGER=1
  ODBCPP_ODBC2_DRIVER_PATH="$<TARGET_FILE:odbcpp_driver>")
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
