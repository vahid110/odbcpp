if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "SOURCE_DIR and BINARY_DIR are required")
endif()

set(_artifact "${BINARY_DIR}/crypto-inspection-fixture.bin")
file(WRITE "${_artifact}" "fixture")
set(_script "${SOURCE_DIR}/cmake/InspectCryptoArtifact.cmake")

function(run_fixture name linkage output resolution expect_success)
  set(_fixture_dir "${BINARY_DIR}/crypto-origin-${name}")
  file(MAKE_DIRECTORY "${_fixture_dir}")
  set(_ssl "${_fixture_dir}/libssl.so.3")
  set(_crypto "${_fixture_dir}/libcrypto.so.3")
  file(WRITE "${_ssl}" "ssl")
  file(WRITE "${_crypto}" "crypto")
  string(REPLACE "@SSL@" "${_ssl}" resolution "${resolution}")
  string(REPLACE "@CRYPTO@" "${_crypto}" resolution "${resolution}")
  set(_manifest "${_fixture_dir}/manifest.json")
  file(WRITE "${_manifest}"
    "{\"provider\":\"OPENSSL\",\"requestedLinkage\":\"${linkage}\","
    "\"dependencyRoot\":\"${_fixture_dir}\","
    "\"sslLinkInput\":\"${_ssl}\",\"cryptoLinkInput\":\"${_crypto}\"}\n")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DARTIFACT=${_artifact}
            -DEXPECTED_PROVIDER=OPENSSL
            -DEXPECTED_LINKAGE=${linkage}
            -DPLATFORM=Linux
            "-DINSPECTION_OUTPUT=${output}"
            "-DRESOLUTION_OUTPUT=${resolution}"
            -DCONFIG_MANIFEST=${_manifest}
            -DEVIDENCE=${_fixture_dir}/evidence.json
            -P "${_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(expect_success AND NOT _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly failed: ${_stderr}${_stdout}")
  elseif(NOT expect_success AND _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly passed")
  endif()
  if(expect_success)
    file(READ "${_fixture_dir}/evidence.json" _evidence)
    string(JSON _schema GET "${_evidence}" schemaVersion)
    string(JSON _origin GET "${_evidence}" dependencyOriginVerified)
    string(JSON _inputs GET "${_evidence}" configuredLinkInputsVerified)
    string(JSON _qualified GET "${_evidence}" qualificationClaimed)
    set(_claims_wrong false)
    if(linkage STREQUAL "BUNDLED_STATIC")
      if(_origin OR NOT _inputs)
        set(_claims_wrong true)
      endif()
    elseif(NOT _origin OR _inputs)
      set(_claims_wrong true)
    endif()
    if(NOT _schema EQUAL 2 OR _claims_wrong OR _qualified)
      message(FATAL_ERROR "${name} emitted incorrect origin evidence")
    endif()
  endif()
endfunction()

run_fixture(shared_ok SYSTEM_SHARED
  "NEEDED libssl.so.3\nNEEDED libcrypto.so.3"
  "libssl.so.3 => @SSL@\nlibcrypto.so.3 => @CRYPTO@" true)
run_fixture(static_ok BUNDLED_STATIC "NEEDED libc.so.6" "" true)
run_fixture(shared_missing_crypto SYSTEM_SHARED "NEEDED libssl.so.3" "" false)
run_fixture(static_has_crypto BUNDLED_STATIC
  "NEEDED libssl.so.3\nNEEDED libcrypto.so.3" "" false)
run_fixture(shared_unresolved SYSTEM_SHARED
  "NEEDED libssl.so.3\nNEEDED libcrypto.so.3"
  "libssl.so.3 => not-found\nlibcrypto.so.3 => @CRYPTO@" false)

set(_controlled "${BINARY_DIR}/crypto-origin-static-escape/controlled")
set(_outside "${BINARY_DIR}/crypto-origin-static-escape/outside")
file(MAKE_DIRECTORY "${_controlled}" "${_outside}")
file(WRITE "${_outside}/libssl.a" "ssl")
file(WRITE "${_controlled}/libcrypto.a" "crypto")
set(_escape_manifest "${BINARY_DIR}/crypto-origin-static-escape/manifest.json")
file(WRITE "${_escape_manifest}"
  "{\"provider\":\"OPENSSL\",\"requestedLinkage\":\"BUNDLED_STATIC\","
  "\"dependencyRoot\":\"${_controlled}\","
  "\"sslLinkInput\":\"${_outside}/libssl.a\","
  "\"cryptoLinkInput\":\"${_controlled}/libcrypto.a\"}\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -DARTIFACT=${_artifact}
          -DEXPECTED_PROVIDER=OPENSSL
          -DEXPECTED_LINKAGE=BUNDLED_STATIC
          -DPLATFORM=Linux
          "-DINSPECTION_OUTPUT=NEEDED libc.so.6"
          -DCONFIG_MANIFEST=${_escape_manifest}
          -P "${_script}"
  RESULT_VARIABLE _escape_result)
if(_escape_result EQUAL 0)
  message(FATAL_ERROR "Bundled-static origin accepted an input outside its root")
endif()

set(_mismatch_manifest "${BINARY_DIR}/crypto-origin-profile-mismatch.json")
file(WRITE "${_mismatch_manifest}"
  "{\"provider\":\"OPENSSL\",\"requestedLinkage\":\"BUNDLED_STATIC\"}\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -DARTIFACT=${_artifact}
          -DEXPECTED_PROVIDER=OPENSSL
          -DEXPECTED_LINKAGE=SYSTEM_SHARED
          -DPLATFORM=Linux
          "-DINSPECTION_OUTPUT=NEEDED libssl.so.3\nNEEDED libcrypto.so.3"
          -DCONFIG_MANIFEST=${_mismatch_manifest}
          -P "${_script}"
  RESULT_VARIABLE _mismatch_result)
if(_mismatch_result EQUAL 0)
  message(FATAL_ERROR "Artifact evidence accepted a mismatched configure profile")
endif()

function(run_windows_fixture name output expect_success)
  set(_fixture_dir "${BINARY_DIR}/crypto-windows-${name}")
  file(MAKE_DIRECTORY "${_fixture_dir}")
  set(_manifest "${_fixture_dir}/manifest.json")
  file(WRITE "${_manifest}"
    "{\"provider\":\"OPENSSL\",\"requestedLinkage\":\"BUNDLED_SHARED\"}\n")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DARTIFACT=${_artifact}
            -DEXPECTED_PROVIDER=OPENSSL
            -DEXPECTED_LINKAGE=BUNDLED_SHARED
            -DPLATFORM=Windows
            "-DINSPECTION_OUTPUT=${output}"
            -DCONFIG_MANIFEST=${_manifest}
            -DEVIDENCE=${_fixture_dir}/evidence.json
            -P "${_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(expect_success AND NOT _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly failed: ${_stderr}${_stdout}")
  elseif(NOT expect_success AND _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly passed")
  endif()
  if(expect_success)
    file(READ "${_fixture_dir}/evidence.json" _evidence)
    string(JSON _count LENGTH "${_evidence}" runtimeDependencies)
    string(JSON _first_role GET "${_evidence}" runtimeDependencies 0 role)
    string(JSON _first_file GET "${_evidence}" runtimeDependencies 0 file)
    string(JSON _origin GET "${_evidence}" dependencyOriginVerified)
    if(NOT _count EQUAL 2 OR NOT _first_role STREQUAL "ssl" OR
       NOT _first_file STREQUAL "LiBsSl-3-X64.DlL" OR _origin)
      message(FATAL_ERROR "${name} emitted incorrect PE import evidence")
    endif()
  endif()
endfunction()

run_windows_fixture(pe_imports_ok
  "Image has dependencies:\n  LiBsSl-3-X64.DlL\n  libcrypto-3-x64.dll\n  KERNEL32.dll" true)
run_windows_fixture(pe_missing_crypto "  libssl-3-x64.dll" false)
run_windows_fixture(pe_duplicate_ssl
  "  libssl-3-x64.dll\n  LIBSSL-3-X64.DLL\n  libcrypto-3-x64.dll" false)
run_windows_fixture(pe_lookalike
  "  libssl-helper.dll\n  libcrypto-3-x64.dll" false)
run_windows_fixture(pe_path_bearing
  "  C:/foreign/libssl-3-x64.dll\n  libcrypto-3-x64.dll" false)
