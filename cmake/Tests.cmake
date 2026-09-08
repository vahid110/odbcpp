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
function(add_test_executable test_name test_file)
  add_executable(${test_name} ${test_file})
  target_link_libraries(${test_name} PRIVATE 
    ${PROJECT_NAME}::core 
    GTest::gtest_main
  )
  apply_compiler_settings(${test_name})
  
  add_test(NAME ${test_name} COMMAND ${test_name})
  
  # Set output directory
  set_target_properties(${test_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
  )
endfunction()

# ---- Unit Tests ----
file(GLOB UNIT_TEST_SOURCES "tests/unit/*.cpp")
foreach(test_file ${UNIT_TEST_SOURCES})
  get_filename_component(test_name ${test_file} NAME_WE)
  add_test_executable(${test_name} ${test_file})
  set_tests_properties(${test_name} PROPERTIES LABELS "unit")
endforeach()

# ---- Integration Tests ----
file(GLOB INTEGRATION_TEST_SOURCES "tests/integration/*.cpp")
foreach(test_file ${INTEGRATION_TEST_SOURCES})
  get_filename_component(test_name ${test_file} NAME_WE)
  add_test_executable(${test_name} ${test_file})
  set_tests_properties(${test_name} PROPERTIES LABELS "integration")
endforeach()

# Exercise the shared driver through the platform ODBC Driver Manager. This
# deliberately does not link odbcpp_core, so missing exports or registration
# problems cannot be hidden by the in-process integration tests.
if(UNIX)
  find_library(ODBC_DRIVER_MANAGER_LIBRARY NAMES odbc iodbc)
  if(ODBC_DRIVER_MANAGER_LIBRARY)
    add_executable(it_driver_manager tests/driver_manager/it_driver_manager.cpp)
    target_include_directories(it_driver_manager PRIVATE ${ODBC_INCLUDE_DIR})
    target_link_libraries(it_driver_manager PRIVATE ${ODBC_DRIVER_MANAGER_LIBRARY})
    apply_compiler_settings(it_driver_manager)
    add_test(NAME it_driver_manager COMMAND it_driver_manager)
    set_tests_properties(it_driver_manager PROPERTIES LABELS "integration")
    set_target_properties(it_driver_manager PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
  endif()
endif()
