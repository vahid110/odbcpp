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
file(MAKE_DIRECTORY "${_root}/foreign")

# Config-only stub for ZLIB: the synthetic SDK fixtures never link zlib, so the
# probes must not depend on a host zlib install (notably absent on Windows CI).
file(MAKE_DIRECTORY "${_root}/stub")
file(WRITE "${_root}/stub/FindZLIB.cmake"
  "if(NOT TARGET ZLIB::ZLIB)\n  add_library(ZLIB::ZLIB INTERFACE IMPORTED)\nendif()\nset(ZLIB_FOUND TRUE)\nset(ZLIB_INCLUDE_DIRS \"\")\nset(ZLIB_LIBRARIES ZLIB::ZLIB)\n")

# Create a synthetic SDK install. Only the discovery/guard inputs are modelled;
# no real SDK files or binaries exist. shared marks the service targets shared;
# core_shared marks core/CRT shared; debug_dir/core_dir select the archive
# directories used for the Debug and core/CRT locations respectively (foreign
# dirs live under ${_root}/foreign, outside the selected prefix).
function(_odbcpp_fake_install prefix version shared core_shared debug_dir core_dir)
  file(MAKE_DIRECTORY "${prefix}/include/aws/sts")
  file(MAKE_DIRECTORY "${prefix}/include/aws/redshift")
  file(MAKE_DIRECTORY "${prefix}/include/aws/redshift-serverless")
  file(MAKE_DIRECTORY "${prefix}/include/aws/sso-oidc")
  file(WRITE "${prefix}/include/aws/sts/STSClient.h" "// synthetic import fixture\n")
  file(WRITE "${prefix}/include/aws/redshift/RedshiftClient.h" "// synthetic import fixture\n")
  file(WRITE "${prefix}/include/aws/redshift-serverless/RedshiftServerlessClient.h" "// synthetic import fixture\n")
  file(WRITE "${prefix}/include/aws/sso-oidc/SSOOIDCClient.h" "// synthetic import fixture\n")
  foreach(_c IN ITEMS redshift redshift-serverless sts sso-oidc)
    file(MAKE_DIRECTORY "${prefix}/lib/cmake/aws-cpp-sdk-${_c}")
    set(_libtype "STATIC")
    if(shared)
      set(_libtype "SHARED")
    endif()
    file(WRITE "${prefix}/lib/cmake/aws-cpp-sdk-${_c}/aws-cpp-sdk-${_c}-config.cmake"
      "include(\"\${CMAKE_CURRENT_LIST_DIR}/aws-cpp-sdk-${_c}-targets.cmake\")\n")
    file(WRITE "${prefix}/lib/cmake/aws-cpp-sdk-${_c}/aws-cpp-sdk-${_c}-targets.cmake"
      "add_library(aws-cpp-sdk-${_c} ${_libtype} IMPORTED)\nset_target_properties(aws-cpp-sdk-${_c} PROPERTIES IMPORTED_CONFIGURATIONS RELEASE\;DEBUG IMPORTED_LOCATION_RELEASE \"${prefix}/lib/libaws-cpp-sdk-${_c}.a\" IMPORTED_LOCATION_DEBUG \"${debug_dir}/libaws-cpp-sdk-${_c}-d.a\")\n")
  endforeach()
  # aws-cpp-sdk-core (un-namespaced, like the real SDK).
  set(_c "aws-cpp-sdk-core")
  file(MAKE_DIRECTORY "${prefix}/lib/cmake/${_c}")
  set(_libtype "STATIC")
  if(shared OR core_shared)
    set(_libtype "SHARED")
  endif()
  file(WRITE "${prefix}/lib/cmake/${_c}/${_c}-config.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/${_c}-targets.cmake\")\n")
  file(WRITE "${prefix}/lib/cmake/${_c}/${_c}-targets.cmake"
    "add_library(${_c} ${_libtype} IMPORTED)\nset_target_properties(${_c} PROPERTIES IMPORTED_CONFIGURATIONS RELEASE IMPORTED_LOCATION_RELEASE \"${core_dir}/lib${_c}.a\")\n")
  # aws-crt-cpp (namespaced AWS::aws-crt-cpp, like the real SDK).
  set(_c "aws-crt-cpp")
  file(MAKE_DIRECTORY "${prefix}/lib/cmake/${_c}")
  set(_libtype "STATIC")
  if(shared OR core_shared)
    set(_libtype "SHARED")
  endif()
  file(WRITE "${prefix}/lib/cmake/${_c}/${_c}-config.cmake"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/${_c}-targets.cmake\")\n")
  file(WRITE "${prefix}/lib/cmake/${_c}/${_c}-targets.cmake"
    "add_library(AWS::aws-crt-cpp ${_libtype} IMPORTED)\nset_target_properties(AWS::aws-crt-cpp PROPERTIES IMPORTED_CONFIGURATIONS RELEASE IMPORTED_LOCATION_RELEASE \"${core_dir}/libaws-crt-cpp.a\")\n")
  file(MAKE_DIRECTORY "${prefix}/lib/cmake/AWSSDK")
  file(WRITE "${prefix}/lib/cmake/AWSSDK/AWSSDKConfigVersion.cmake"
    "set(PACKAGE_VERSION \"${version}\")\nif(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)\n  set(PACKAGE_VERSION_COMPATIBLE FALSE)\nelse()\n  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n  if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)\n    set(PACKAGE_VERSION_EXACT TRUE)\n  endif()\nendif()\n")
  set(_shared_off "OFF")
  if(shared)
    set(_shared_off "ON")
  endif()
  file(WRITE "${prefix}/lib/cmake/AWSSDK/AWSSDKConfig.cmake"
    "set(AWSSDK_FOUND TRUE)\nset(AWSSDK_VERSION \"${version}\")\nset(AWSSDK_INSTALL_AS_SHARED_LIBS ${_shared_off})\nset(AWSSDK_redshift_FOUND TRUE)\nset(AWSSDK_redshift-serverless_FOUND TRUE)\nset(AWSSDK_sts_FOUND TRUE)\nset(AWSSDK_sso-oidc_FOUND TRUE)\nforeach(_c IN ITEMS redshift redshift-serverless sts sso-oidc)\n  include(\"\${CMAKE_CURRENT_LIST_DIR}/../aws-cpp-sdk-\${_c}/aws-cpp-sdk-\${_c}-config.cmake\")\nendforeach()\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/../aws-cpp-sdk-core/aws-cpp-sdk-core-config.cmake\")\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/../aws-crt-cpp/aws-crt-cpp-config.cmake\")\n")
  # Create the (empty) archive files so file(REAL_PATH) can resolve symlinks.
  file(MAKE_DIRECTORY "${prefix}/lib")
  file(MAKE_DIRECTORY "${debug_dir}")
  file(MAKE_DIRECTORY "${core_dir}")
  foreach(_c IN ITEMS redshift redshift-serverless sts sso-oidc)
    file(WRITE "${prefix}/lib/libaws-cpp-sdk-${_c}.a" "")
    file(WRITE "${debug_dir}/libaws-cpp-sdk-${_c}-d.a" "")
  endforeach()
  file(WRITE "${core_dir}/libaws-cpp-sdk-core.a" "")
  file(WRITE "${core_dir}/libaws-crt-cpp.a" "")
endfunction()

set(_a "${_root}/install-a")
set(_b "${_root}/install-b")
set(_c "${_root}/install-c")
set(_d "${_root}/install-d")
set(_e "${_root}/install-e")
set(_f "${_root}/install-f")
set(_g "${_root}/install-g")
_odbcpp_fake_install("${_a}" "1.11.906" OFF OFF "${_a}/lib" "${_a}/lib")
_odbcpp_fake_install("${_b}" "1.11.906" OFF OFF "${_b}/lib" "${_b}/lib")
_odbcpp_fake_install("${_c}" "1.11.900" OFF OFF "${_c}/lib" "${_c}/lib")
_odbcpp_fake_install("${_d}" "1.11.906" ON  OFF "${_d}/lib" "${_d}/lib")
_odbcpp_fake_install("${_e}" "1.11.906" OFF OFF "${_e}/lib" "${_root}/foreign")
_odbcpp_fake_install("${_f}" "1.11.906" OFF OFF "${_root}/foreign" "${_f}/lib")
_odbcpp_fake_install("${_g}" "1.11.906" OFF ON  "${_g}/lib" "${_g}/lib")

function(_odbcpp_probe name prefix expect_success extra_setup)
  set(_dir "${_root}/${name}")
  file(MAKE_DIRECTORY "${_dir}")
  file(WRITE "${_dir}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.20)\nproject(${name} NONE)\nset(ODBCPP_AWS_SDK_INSTALL \"${prefix}\" CACHE PATH \"\" FORCE)\nlist(PREPEND CMAKE_MODULE_PATH \"${_root}/stub\")\n${extra_setup}include(\"${SOURCE_DIR}/cmake/NativeAwsSdkImport.cmake\")\nmessage(STATUS \"${name}: IMPORT OK\")\n")
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

# Valid prefix A configures cleanly (service + core + CRT closure in-prefix).
_odbcpp_probe("probe-valid" "${_a}" TRUE "")

# A cached AWSSDK_DIR pointing at B while the prefix is A must be refused.
_odbcpp_probe("probe-cached-conflict" "${_a}" FALSE
  "set(AWSSDK_DIR \"${_b}/lib/cmake/AWSSDK\" CACHE PATH \"\" FORCE)\n")

# Wrong version and wrong service linkage are rejected.
_odbcpp_probe("probe-wrong-version" "${_c}" FALSE "")
_odbcpp_probe("probe-shared-linkage" "${_d}" FALSE "")

# Foreign core/CRT archive (service in A, core in the foreign dir) is refused.
_odbcpp_probe("probe-foreign-core" "${_e}" FALSE "")

# Foreign Debug archive (Release in A, Debug in the foreign dir) is refused.
_odbcpp_probe("probe-foreign-debug" "${_f}" FALSE "")

# Shared core/CRT linkage (service static, core shared) is refused.
_odbcpp_probe("probe-shared-core" "${_g}" FALSE "")

# Archive symlink: the core archive inside H is symlinked to a foreign file.
# Canonical (symlink-resolving) containment must refuse it. Symlink creation is
# platform-dependent (Windows symlinks need elevated/developer-mode privileges),
# so these probes are Unix-only.
if(NOT WIN32)
  set(_h "${_root}/install-h")
  _odbcpp_fake_install("${_h}" "1.11.906" OFF OFF "${_h}/lib" "${_h}/lib")
  file(WRITE "${_root}/foreign/foreign-core.a" "")
  file(REMOVE "${_h}/lib/libaws-cpp-sdk-core.a")
  file(CREATE_LINK "${_root}/foreign/foreign-core.a" "${_h}/lib/libaws-cpp-sdk-core.a" SYMBOLIC)
  _odbcpp_probe("probe-archive-symlink" "${_h}" FALSE "")

  # Parent-directory symlink: the core archive's parent directory is symlinked to
  # a foreign lib directory; canonical resolution must refuse it.
  set(_i "${_root}/install-i")
  _odbcpp_fake_install("${_i}" "1.11.906" OFF OFF "${_i}/lib" "${_i}/subdir")
  file(MAKE_DIRECTORY "${_root}/foreign-lib")
  file(WRITE "${_root}/foreign-lib/libaws-cpp-sdk-core.a" "")
  file(WRITE "${_root}/foreign-lib/libaws-crt-cpp.a" "")
  file(REMOVE_RECURSE "${_i}/subdir")
  file(CREATE_LINK "${_root}/foreign-lib" "${_i}/subdir" SYMBOLIC)
  _odbcpp_probe("probe-parent-symlink" "${_i}" FALSE "")
endif()

message(STATUS "NativeAwsSdkImport guards verified")
