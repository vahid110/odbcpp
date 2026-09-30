# Linux-only actual ODBC artifact; does not enable the public AWS_LC profile.
include("${ODBCPP_SOURCE}/cmake/DriverSources.cmake")
include("${ODBCPP_SOURCE}/cmake/FindLogging.cmake")
include("${ODBCPP_SOURCE}/cmake/CompilerSettings.cmake")
find_path(PROOF_ODBC_INCLUDE sql.h REQUIRED)
find_library(PROOF_ODBC_LIBRARY odbc REQUIRED)
odbcpp_collect_driver_sources(_driver_sources POSTGRESQL)
add_library(awslc_odbc_driver SHARED ${_driver_sources})
target_include_directories(awslc_odbc_driver PRIVATE "${ODBCPP_SOURCE}" "${PROOF_ODBC_INCLUDE}")
target_include_directories(awslc_odbc_driver SYSTEM PRIVATE "${SPDLOG_INCLUDE_DIR}")
target_compile_definitions(awslc_odbc_driver PRIVATE ODBCPP_ENABLE_POSTGRESQL=1 ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE=2)
target_link_libraries(awslc_odbc_driver PRIVATE ssl crypto Threads::Threads)
apply_compiler_settings(awslc_odbc_driver)
set(_exports "${ODBCPP_SOURCE}/odbc/odbcpp.exports")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_exports}")
file(STRINGS "${_exports}" _symbols)
list(FILTER _symbols EXCLUDE REGEX "^[ \t]*(#.*)?$")
list(TRANSFORM _symbols STRIP)
set(_map "${CMAKE_CURRENT_BINARY_DIR}/driver-exports.map")
file(WRITE "${_map}" "{\n global:\n")
foreach(_symbol IN LISTS _symbols)
  if(NOT _symbol MATCHES "^SQL[A-Za-z0-9_]+$")
    message(FATAL_ERROR "Invalid ODBC export in proof: ${_symbol}")
  endif()
  file(APPEND "${_map}" "  ${_symbol};\n")
endforeach()
file(APPEND "${_map}" " local: *;\n};\n")
target_link_options(awslc_odbc_driver PRIVATE "LINKER:--version-script,${_map}")
set_property(TARGET awslc_odbc_driver APPEND PROPERTY LINK_DEPENDS "${_map}")
if(NOT BUILD_SHARED_LIBS)
  target_link_options(awslc_odbc_driver PRIVATE "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/driver-link.map")
endif()
find_program(NM_EXECUTABLE nm REQUIRED)
add_test(NAME odbc_driver_exports COMMAND "${CMAKE_COMMAND}"
  "-DARTIFACT=$<TARGET_FILE:awslc_odbc_driver>" "-DEXPECTED=${_exports}"
  "-DINSPECTOR=${NM_EXECUTABLE}" -DPLATFORM=Linux
  -P "${ODBCPP_SOURCE}/cmake/CheckDriverExports.cmake")
add_test(NAME odbc_driver_linkage COMMAND "${CMAKE_COMMAND}"
  "-DARTIFACT=$<TARGET_FILE:awslc_odbc_driver>" -DEXPECTED_PROVIDER=AWS_LC
  "-DEXPECTED_LINKAGE=${_proof_linkage}" -DPLATFORM=Linux
  "-DINSPECTOR=${READELF_EXECUTABLE}" "-DRESOLVER=${LDD_EXECUTABLE}"
  "-DCONFIG_MANIFEST=${CMAKE_CURRENT_BINARY_DIR}/linkage-manifest.json"
  "-DSTATIC_LINK_MAP=${CMAKE_CURRENT_BINARY_DIR}/driver-link.map"
  "-DSTATIC_LINKER_IDENTITY=${_linker_identity}"
  "-DEVIDENCE=${CMAKE_CURRENT_BINARY_DIR}/driver-artifact-evidence.json"
  -P "${ODBCPP_SOURCE}/cmake/InspectCryptoArtifact.cmake")
add_executable(test_odbc_driver_live "${ODBCPP_SOURCE}/tests/driver_manager/it_crypto_profile_live.cpp")
target_include_directories(test_odbc_driver_live PRIVATE "${PROOF_ODBC_INCLUDE}")
target_link_libraries(test_odbc_driver_live PRIVATE GTest::gtest_main "${PROOF_ODBC_LIBRARY}" ${CMAKE_DL_LIBS})
target_compile_definitions(test_odbc_driver_live PRIVATE
  "ODBCPP_DRIVER_LIBRARY_PATH=\"$<TARGET_FILE:awslc_odbc_driver>\"")
add_dependencies(test_odbc_driver_live awslc_odbc_driver)
if(ODBCPP_AWSLC_LIVE_TESTS)
  add_test(NAME odbc_driver_live COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/test_odbc_driver.py"
    $<TARGET_FILE:test_odbc_driver_live> $<TARGET_FILE:awslc_odbc_driver>)
  set_tests_properties(odbc_driver_live PROPERTIES TIMEOUT 90
    ENVIRONMENT "LD_PRELOAD=${ODBCPP_HOST_SSL}:${ODBCPP_HOST_CRYPTO}")
endif()
