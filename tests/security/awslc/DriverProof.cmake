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

# Component installation keeps this experiment separate from upstream headers,
# tools and the project's production package. Runtime paths are package-relative.
set_target_properties(awslc_odbc_driver PROPERTIES INSTALL_RPATH "$ORIGIN"
  INSTALL_RPATH_USE_LINK_PATH FALSE)
install(TARGETS awslc_odbc_driver LIBRARY DESTINATION lib COMPONENT awslc-proof)
if(BUILD_SHARED_LIBS)
  set_target_properties(ssl crypto PROPERTIES INSTALL_RPATH "$ORIGIN"
    INSTALL_RPATH_USE_LINK_PATH FALSE)
  install(TARGETS ssl crypto LIBRARY DESTINATION lib COMPONENT awslc-proof)
endif()
install(FILES "${awslc_SOURCE_DIR}/LICENSE" "${awslc_SOURCE_DIR}/NOTICE"
  DESTINATION licenses/aws-lc COMPONENT awslc-proof)
install(FILES "${spdlog_SOURCE_DIR}/LICENSE" DESTINATION licenses/spdlog COMPONENT awslc-proof)
file(READ "${SPDLOG_INCLUDE_DIR}/spdlog/fmt/bundled/format.h" _fmt_header)
string(FIND "${_fmt_header}" "*/" _fmt_license_end)
if(NOT _fmt_header MATCHES "^/\\*" OR _fmt_license_end LESS 0)
  message(FATAL_ERROR "Pinned fmt license header could not be identified")
endif()
math(EXPR _fmt_license_length "${_fmt_license_end} + 2")
string(SUBSTRING "${_fmt_header}" 0 ${_fmt_license_length} _fmt_license)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fmt-LICENSE" "${_fmt_license}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/fmt-LICENSE"
  DESTINATION licenses/fmt RENAME LICENSE COMPONENT awslc-proof)

file(SHA256 "${awslc_SOURCE_DIR}/LICENSE" AWSLC_LICENSE_HASH)
file(SHA256 "${awslc_SOURCE_DIR}/NOTICE" AWSLC_NOTICE_HASH)
file(SHA256 "${spdlog_SOURCE_DIR}/LICENSE" SPDLOG_LICENSE_HASH)
file(SHA256 "${CMAKE_CURRENT_BINARY_DIR}/fmt-LICENSE" FMT_LICENSE_HASH)
file(STRINGS "${SPDLOG_INCLUDE_DIR}/spdlog/fmt/bundled/base.h" _fmt_version_line
  REGEX "^#define FMT_VERSION [0-9]+$")
if(NOT _fmt_version_line MATCHES "^#define FMT_VERSION ([0-9]+)$")
  message(FATAL_ERROR "Pinned bundled fmt version could not be identified")
endif()
set(FMT_VERSION_NUMBER "${CMAKE_MATCH_1}")
# Declared archive recipes are distinct from a verified source-tree provenance
# claim, especially when FetchContent source overrides are used locally.
foreach(_dependency IN ITEMS AWSLC SPDLOG)
  if(FETCHCONTENT_SOURCE_DIR_${_dependency})
    set(${_dependency}_SOURCE_OVERRIDE true)
  else()
    set(${_dependency}_SOURCE_OVERRIDE false)
  endif()
endforeach()
file(STRINGS "${awslc_SOURCE_DIR}/include/openssl/base.h" _aws_version_line
  REGEX "^#define AWSLC_VERSION_NUMBER_STRING ")
if(NOT _aws_version_line MATCHES "^#define AWSLC_VERSION_NUMBER_STRING \"([0-9]+\\.[0-9]+\\.[0-9]+)\"$")
  message(FATAL_ERROR "AWS-LC compile version could not be identified")
endif()
set(AWSLC_COMPILE_VERSION "AWS-LC ${CMAKE_MATCH_1}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/package-manifest.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/package-manifest.json" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/package-manifest.json"
  DESTINATION . COMPONENT awslc-proof)

if(ODBCPP_AWSLC_LIVE_TESTS)
  add_test(NAME relocated_odbc_package COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/test_relocated_package.py"
    --build "${CMAKE_CURRENT_BINARY_DIR}" --cmake "${CMAKE_COMMAND}"
    --readelf "${READELF_EXECUTABLE}" --shared "${BUILD_SHARED_LIBS}"
    --test $<TARGET_FILE:test_odbc_driver_live>)
  set_tests_properties(relocated_odbc_package PROPERTIES TIMEOUT 120
    ENVIRONMENT "LD_PRELOAD=${ODBCPP_HOST_SSL}:${ODBCPP_HOST_CRYPTO}")
endif()
