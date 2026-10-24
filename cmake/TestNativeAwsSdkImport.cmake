# Offline config-only regression for the pinned AWS SDK import recipe. No SDK
# build, network, or compiler work: synthetic install fixtures exercise only the
# discovery/version/linkage/location guards. This is not an SDK build or a live
# qualification; it validates the git-pinned recipe's refusal boundaries.
if(NOT DEFINED SOURCE_DIR OR NOT EXISTS "${SOURCE_DIR}/cmake/NativeAwsSdkImport.cmake")
  message(FATAL_ERROR "SOURCE_DIR is required and must contain the import recipe")
endif()
if(NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "BINARY_DIR is required")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _nonce)
set(_root "${BINARY_DIR}/aws-sdk-import-fixtures-${_nonce}")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}")

# Create a synthetic SDK install with the given version and linkage. Only the
# discovery/guard inputs are modelled; no real SDK files or binaries exist.
function(_odbcpp_fake_install prefix version shared)
  file(MAKE_DIRECTORY "${prefix}/include/aws/sts")
  file(MAKE_DIRECTORY "${prefix}/include/aws/redshift")
  file(MAKE_DIRECTORY "${prefix}/include/aws/redshift-serverless")
  file(WRITE "${prefix}/include/aws/sts/STSClient.h" "// synthetic import fixture\n")
  file(WRITE "${prefix}/include/aws/redshift/RedshiftClient.h" "// synthetic import fixture\n")
  file(WRITE "${prefix}/include/aws/redshift-serverless/RedshiftServerlessClient.h" "// synthetic import fixture\n")
  foreach(_c IN ITEMS redshift redshift-serverless sts)
    file(MAKE_DIRECTORY "${prefix}/lib/cmake/aws-cpp-sdk-${_c}")
    set(_libtype "STATIC")
    if(shared)
      set(_libtype "SHARED")
    endif()
    file(WRITE "${prefix}/lib/cmake/aws-cpp-sdk-${_c}/aws-cpp-sdk-${_c}-config.cmake"
      "include(\"\${CMAKE_CURRENT_LIST_DIR}/aws-cpp-sdk-${_c}-targets.cmake\")\n")
    file(WRITE "${prefix}/lib/cmake/aws-cpp-sdk-${_c}/aws-cpp-sdk-${_c}-targets.cmake"
      "add_library(aws-cpp-sdk-${_c} ${_libtype} IMPORTED)\nset_target_properties(aws-cpp-sdk-${_c} PROPERTIES IMPORTED_LOCATION_RELEASE \"${prefix}/lib/libaws-cpp-sdk-${_c}.a\")\n")
  endforeach()
  file(MAKE_DIRECTORY "${prefix}/lib/cmake/AWSSDK")
  file(WRITE "${prefix}/lib/cmake/AWSSDK/AWSSDKConfigVersion.cmake"
    "set(PACKAGE_VERSION \"${version}\")\nif(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)\n  set(PACKAGE_VERSION_COMPATIBLE FALSE)\nelse()\n  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n  if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)\n    set(PACKAGE_VERSION_EXACT TRUE)\n  endif()\nendif()\n")
  set(_shared_off "OFF")
  if(shared)
    set(_shared_off "ON")
  endif()
  file(WRITE "${prefix}/lib/cmake/AWSSDK/AWSSDKConfig.cmake"
    "set(AWSSDK_FOUND TRUE)\nset(AWSSDK_VERSION \"${version}\")\nset(AWSSDK_INSTALL_AS_SHARED_LIBS ${_shared_off})\nset(AWSSDK_redshift_FOUND TRUE)\nset(AWSSDK_redshift-serverless_FOUND TRUE)\nset(AWSSDK_sts_FOUND TRUE)\nforeach(_c IN ITEMS redshift redshift-serverless sts)\n  include(\"\${CMAKE_CURRENT_LIST_DIR}/../aws-cpp-sdk-\${_c}/aws-cpp-sdk-\${_c}-config.cmake\")\nendforeach()\n")
endfunction()

# Four distinguishable fixtures: a valid reviewed install (A), a second valid
# install (B) used only as a stale cached AWSSDK_DIR, a wrong-version install
# (C), and a wrong-linkage install (D).
set(_a "${_root}/install-a")
set(_b "${_root}/install-b")
set(_c "${_root}/install-c")
set(_d "${_root}/install-d")
_odbcpp_fake_install("${_a}" "1.11.906" OFF)
_odbcpp_fake_install("${_b}" "1.11.906" OFF)
_odbcpp_fake_install("${_c}" "1.11.900" OFF)
_odbcpp_fake_install("${_d}" "1.11.906" ON)

function(_odbcpp_probe name prefix expect_success extra_setup)
  set(_dir "${_root}/${name}")
  file(MAKE_DIRECTORY "${_dir}")
  file(WRITE "${_dir}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.20)\nproject(${name} LANGUAGES CXX)\nset(ODBCPP_AWS_SDK_INSTALL \"${prefix}\" CACHE PATH \"\" FORCE)\n${extra_setup}include(\"${SOURCE_DIR}/cmake/NativeAwsSdkImport.cmake\")\nmessage(STATUS \"${name}: IMPORT OK\")\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_dir}" -B "${_dir}/build"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr TIMEOUT 60)
  file(WRITE "${_dir}/configure.log" "${_stdout}\n${_stderr}")
  if(expect_success AND NOT _result STREQUAL "0")
    message(FATAL_ERROR "${name} should have configured successfully but refused:\n${_stdout}\n${_stderr}")
  endif()
  if(NOT expect_success AND _result STREQUAL "0")
    message(FATAL_ERROR "${name} should have been refused but configured successfully")
  endif()
endfunction()

# Valid prefix A configures cleanly.
_odbcpp_probe("probe-valid" "${_a}" TRUE "")

# A cached AWSSDK_DIR pointing at B while the prefix is A must be refused.
_odbcpp_probe("probe-cached-conflict" "${_a}" FALSE
  "set(AWSSDK_DIR \"${_b}/lib/cmake/AWSSDK\" CACHE PATH \"\" FORCE)\n")

# Wrong version and wrong linkage are rejected.
_odbcpp_probe("probe-wrong-version" "${_c}" FALSE "")
_odbcpp_probe("probe-shared-linkage" "${_d}" FALSE "")

message(STATUS "NativeAwsSdkImport guards verified")
