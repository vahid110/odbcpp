cmake_minimum_required(VERSION 3.20)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(ODBCPPContracts LANGUAGES CXX)
  include(CTest)
endif()

# Internal build-only surface. No installed/exported ABI or distribution claim.
get_filename_component(_sdk_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(_sdk_manifest "${CMAKE_CURRENT_SOURCE_DIR}/contract_headers.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sdk_manifest}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${_sdk_root}"
  "-DMANIFEST=${_sdk_manifest}"
  -P "${_sdk_root}/cmake/CheckSdkContracts.cmake"
  RESULT_VARIABLE _sdk_boundary_result)
if(NOT _sdk_boundary_result EQUAL 0)
  message(FATAL_ERROR "SDK contract dependency boundary failed")
endif()

# A fresh allowlisted tree prevents removed entries from lingering after reconfigure.
set(_sdk_include "${CMAKE_CURRENT_BINARY_DIR}/contract-include")
file(REMOVE_RECURSE "${_sdk_include}")
file(STRINGS "${_sdk_manifest}" _sdk_headers)
add_library(odbcpp_sdk_contracts INTERFACE)
add_library(odbcpp::sdk_contracts ALIAS odbcpp_sdk_contracts)
target_compile_features(odbcpp_sdk_contracts INTERFACE cxx_std_20)
target_include_directories(odbcpp_sdk_contracts INTERFACE "${_sdk_include}")
set(_sdk_header_units "")
foreach(_header IN LISTS _sdk_headers)
  configure_file("${_sdk_root}/sdk/include/${_header}" "${_sdk_include}/${_header}" COPYONLY)
  if(BUILD_TESTING)
    string(REPLACE "/" "_" _unit_name "${_header}")
    set(_unit "${CMAKE_CURRENT_BINARY_DIR}/${_unit_name}.cpp")
    file(WRITE "${_unit}" "#include <${_header}>\n")
    list(APPEND _sdk_header_units "${_unit}")
  endif()
endforeach()
if(BUILD_TESTING)
  include("${_sdk_root}/cmake/CompilerSettings.cmake")
  # Each header is the first and only include, without root/private include paths.
  add_library(odbcpp_sdk_header_isolation OBJECT ${_sdk_header_units})
  target_link_libraries(odbcpp_sdk_header_isolation PRIVATE odbcpp::sdk_contracts)
  apply_compiler_settings(odbcpp_sdk_header_isolation)
  add_executable(odbcpp_sdk_contract_consumer tests/contract_consumer.cpp)
  target_link_libraries(odbcpp_sdk_contract_consumer PRIVATE odbcpp::sdk_contracts)
  apply_compiler_settings(odbcpp_sdk_contract_consumer)
  add_library(odbcpp_session_baseline STATIC tests/session_baseline/runner.cpp)
  target_link_libraries(odbcpp_session_baseline PRIVATE odbcpp::sdk_contracts)
  apply_compiler_settings(odbcpp_session_baseline)
  add_executable(odbcpp_session_baseline_synthetic tests/session_baseline/synthetic.cpp)
  target_link_libraries(odbcpp_session_baseline_synthetic PRIVATE odbcpp_session_baseline odbcpp::sdk_contracts)
  apply_compiler_settings(odbcpp_session_baseline_synthetic)
  add_test(NAME test_sdk_session_baseline COMMAND odbcpp_session_baseline_synthetic)
  set_tests_properties(test_sdk_session_baseline PROPERTIES LABELS "unit;architecture" TIMEOUT 30)
  add_test(NAME test_sdk_contract_consumer COMMAND odbcpp_sdk_contract_consumer)
  add_test(NAME test_sdk_contract_boundary COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${_sdk_root}" "-DMANIFEST=${_sdk_manifest}"
    -P "${_sdk_root}/cmake/CheckSdkContracts.cmake")
  add_test(NAME test_sdk_contract_boundary_failures COMMAND "${CMAKE_COMMAND}"
    "-DCHECKER=${_sdk_root}/cmake/CheckSdkContracts.cmake"
    "-DFIXTURE_DIR=${CMAKE_CURRENT_BINARY_DIR}/boundary-fixtures"
    -P "${_sdk_root}/cmake/TestSdkContractBoundary.cmake")
  set_tests_properties(test_sdk_contract_consumer test_sdk_contract_boundary test_sdk_contract_boundary_failures
    PROPERTIES LABELS "unit;architecture" TIMEOUT 30)
endif()
