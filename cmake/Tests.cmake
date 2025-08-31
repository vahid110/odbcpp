# ---- Google Test Setup ----
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
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