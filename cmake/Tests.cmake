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

if(TARGET test_driver_capabilities)
  add_dependencies(test_driver_capabilities odbcpp_driver)
  target_compile_definitions(test_driver_capabilities PRIVATE
    ODBCPP_DRIVER_LIBRARY_PATH="$<TARGET_FILE:odbcpp_driver>")
  if(CMAKE_DL_LIBS)
    target_link_libraries(test_driver_capabilities PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

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
  if(ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "IODBC")
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

  if(NOT ODBC_DRIVER_MANAGER_INCLUDE_DIR)
    message(FATAL_ERROR
      "ODBC driver-manager headers were not found for ${ODBC_DRIVER_MANAGER_FLAVOR}")
  endif()

  find_library(ODBC_DRIVER_MANAGER_LIBRARY NAMES ${_odbc_driver_manager_names})
  if(ODBC_DRIVER_MANAGER_LIBRARY)
    add_executable(it_driver_manager tests/driver_manager/it_driver_manager.cpp)
    target_include_directories(it_driver_manager PRIVATE
      ${ODBC_DRIVER_MANAGER_INCLUDE_DIR})
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
