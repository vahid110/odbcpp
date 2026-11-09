if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "SOURCE_DIR and BINARY_DIR are required")
endif()

set(_artifact "${BINARY_DIR}/crypto-inspection-fixture.bin")
file(WRITE "${_artifact}" "fixture")
set(_script "${SOURCE_DIR}/cmake/InspectCryptoArtifact.cmake")

function(run_fixture name linkage output resolution expect_success static_map_mode)
  set(_provider OPENSSL)
  if(ARGC GREATER 6)
    set(_provider "${ARGV6}")
  endif()
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
    "{\"provider\":\"${_provider}\",\"requestedLinkage\":\"${linkage}\","
    "\"dependencyRoot\":\"${_fixture_dir}\","
    "\"sslLinkInput\":\"${_ssl}\",\"cryptoLinkInput\":\"${_crypto}\"}\n")
  set(_static_map_argument "")
  if(static_map_mode MATCHES "^(GOOD|RELATIVE|PREFIX_ONLY)$")
    set(_static_map "${_fixture_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "OUTPUT(${_artifact} elf64-x86-64)\n"
      "${_ssl}(ssl_member.o)\n${_crypto}(crypto_member.o)\n"
      "LOAD ${_ssl}\nLOAD ${_crypto}\n")
    if(static_map_mode STREQUAL "RELATIVE")
      file(RELATIVE_PATH _relative_root "${BINARY_DIR}" "${_fixture_dir}")
      file(READ "${_static_map}" _map_text)
      string(REPLACE "${_fixture_dir}/" "${_relative_root}/" _map_text "${_map_text}")
      file(WRITE "${_static_map}" "${_map_text}")
    elseif(static_map_mode STREQUAL "PREFIX_ONLY")
      file(READ "${_static_map}" _map_text)
      string(REPLACE "LOAD ${_ssl}\n" "LOAD ${_ssl}-different\n" _map_text "${_map_text}")
      file(WRITE "${_static_map}" "${_map_text}")
    endif()
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  elseif(static_map_mode STREQUAL "SSL_ONLY")
    set(_static_map "${_fixture_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "OUTPUT(${_artifact} elf64-x86-64)\n"
      "${_ssl}(ssl_member.o)\nLOAD ${_ssl}\nLOAD ${_crypto}\n")
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  elseif(static_map_mode STREQUAL "WRONG_OUTPUT")
    set(_static_map "${_fixture_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "OUTPUT(${_fixture_dir}/another-driver.so elf64-x86-64)\n"
      "${_ssl}(ssl_member.o)\n${_crypto}(crypto_member.o)\n"
      "LOAD ${_ssl}\nLOAD ${_crypto}\n")
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  endif()
  if(static_map_mode STREQUAL "WRONG_TARGET")
    file(WRITE "${_fixture_dir}/other-ssl.so" "different ssl")
    file(READ "${_manifest}" _manifest_text)
    string(JSON _manifest_text SET "${_manifest_text}" sslLinkInput
      "\"${_fixture_dir}/other-ssl.so\"")
    file(WRITE "${_manifest}" "${_manifest_text}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DARTIFACT=${_artifact}
            -DEXPECTED_PROVIDER=${_provider}
            -DEXPECTED_LINKAGE=${linkage}
            -DPLATFORM=Linux
            "-DINSPECTION_OUTPUT=${output}"
            "-DRESOLUTION_OUTPUT=${resolution}"
            -DCONFIG_MANIFEST=${_manifest}
            ${_static_map_argument}
            "-DSTATIC_LINKER_IDENTITY=GNU ld (fixture) 2.42"
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
    string(JSON _reported_provider GET "${_evidence}" provider)
    string(JSON _origin GET "${_evidence}" dependencyOriginVerified)
    string(JSON _inputs GET "${_evidence}" configuredLinkInputsVerified)
    string(JSON _map GET "${_evidence}" staticLinkMapVerified)
    string(JSON _members GET "${_evidence}" staticArchiveMemberEvidenceVerified)
    string(JSON _qualified GET "${_evidence}" qualificationClaimed)
    set(_claims_wrong false)
    if(linkage STREQUAL "BUNDLED_STATIC")
      if(_origin OR NOT _inputs OR NOT _map OR NOT _members)
        set(_claims_wrong true)
      endif()
    elseif(NOT _origin OR _inputs OR _map OR _members)
      set(_claims_wrong true)
    endif()
    if(NOT _schema EQUAL 2 OR _claims_wrong OR _qualified OR
       NOT _reported_provider STREQUAL _provider)
      message(FATAL_ERROR "${name} emitted incorrect origin evidence")
    endif()
  endif()
endfunction()

run_fixture(shared_ok SYSTEM_SHARED
  "NEEDED libssl.so.3\nNEEDED libcrypto.so.3"
  "libssl.so.3 => @SSL@\nlibcrypto.so.3 => @CRYPTO@" true NONE)
run_fixture(static_ok BUNDLED_STATIC "NEEDED libc.so.6" "" true GOOD)
run_fixture(static_missing_map BUNDLED_STATIC "NEEDED libc.so.6" "" false NONE)
run_fixture(static_incomplete_map BUNDLED_STATIC "NEEDED libc.so.6" "" false SSL_ONLY)
run_fixture(static_wrong_output BUNDLED_STATIC "NEEDED libc.so.6" "" false WRONG_OUTPUT)
run_fixture(static_relative BUNDLED_STATIC "NEEDED libc.so.6" "" true RELATIVE)
run_fixture(static_prefix_only BUNDLED_STATIC "NEEDED libc.so.6" "" false PREFIX_ONLY)
run_fixture(awslc_shared BUNDLED_SHARED
  "NEEDED libssl.so\nNEEDED libcrypto.so"
  "libssl.so => @SSL@\nlibcrypto.so => @CRYPTO@" true NONE AWS_LC)
run_fixture(awslc_static BUNDLED_STATIC "NEEDED libc.so.6" "" true RELATIVE AWS_LC)
run_fixture(awslc_wrong_target BUNDLED_SHARED
  "NEEDED libssl.so\nNEEDED libcrypto.so"
  "libssl.so => @SSL@\nlibcrypto.so => @CRYPTO@" false WRONG_TARGET AWS_LC)
run_fixture(awslc_missing_crypto BUNDLED_SHARED "NEEDED libssl.so" "" false NONE AWS_LC)
run_fixture(awslc_static_dynamic BUNDLED_STATIC "NEEDED libcrypto.so" "" false GOOD AWS_LC)
run_fixture(awslc_incomplete_map BUNDLED_STATIC "NEEDED libc.so.6" "" false SSL_ONLY AWS_LC)
run_fixture(awslc_wrong_output BUNDLED_STATIC "NEEDED libc.so.6" "" false WRONG_OUTPUT AWS_LC)
run_fixture(awslc_system_disallowed SYSTEM_SHARED
  "NEEDED libssl.so\nNEEDED libcrypto.so"
  "libssl.so => @SSL@\nlibcrypto.so => @CRYPTO@" false NONE AWS_LC)
run_fixture(awslc_foreign_shared BUNDLED_SHARED
  "NEEDED libssl.so\nNEEDED libcrypto.so"
  "libssl.so => ${BINARY_DIR}/crypto-origin-awslc_shared/libssl.so.3\nlibcrypto.so => @CRYPTO@"
  false NONE AWS_LC)
run_fixture(shared_missing_crypto SYSTEM_SHARED "NEEDED libssl.so.3" "" false NONE)
run_fixture(static_has_crypto BUNDLED_STATIC
  "NEEDED libssl.so.3\nNEEDED libcrypto.so.3" "" false GOOD)
run_fixture(shared_unresolved SYSTEM_SHARED
  "NEEDED libssl.so.3\nNEEDED libcrypto.so.3"
  "libssl.so.3 => not-found\nlibcrypto.so.3 => @CRYPTO@" false NONE)

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
  set(_artifact "${_fixture_dir}/openssl/odbcpp.dll")
  file(MAKE_DIRECTORY "${_fixture_dir}/openssl")
  file(WRITE "${_artifact}" "fixture")
  string(REPLACE "@ARTIFACT@" "${_artifact}" output "${output}")
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
run_windows_fixture(pe_imports_artifact_header
  "Dump of file @ARTIFACT@\n\nFile Type: DLL\n  Image has dependencies:\n  LiBsSl-3-X64.DlL\n  libcrypto-3-x64.dll\n  KERNEL32.dll" true)
run_windows_fixture(pe_wrong_artifact_header
  "Dump of file C:/foreign/openssl/odbcpp.dll\n  LiBsSl-3-X64.DlL\n  libcrypto-3-x64.dll" false)
run_windows_fixture(pe_missing_crypto "  libssl-3-x64.dll" false)
run_windows_fixture(pe_duplicate_ssl
  "  libssl-3-x64.dll\n  LIBSSL-3-X64.DLL\n  libcrypto-3-x64.dll" false)
run_windows_fixture(pe_lookalike
  "  libssl-helper.dll\n  libcrypto-3-x64.dll" false)
run_windows_fixture(pe_path_bearing
  "  C:/foreign/libssl-3-x64.dll\n  libcrypto-3-x64.dll" false)

function(run_darwin_fixture name linkage output resolution expect_success static_map_mode)
  set(_fixture_dir "${BINARY_DIR}/crypto-origin-darwin-${name}")
  file(MAKE_DIRECTORY "${_fixture_dir}")
  set(_artifact "${_fixture_dir}/libodbcpp.dylib")
  file(WRITE "${_artifact}" "fixture")
  set(_ssl "${_fixture_dir}/libssl.a")
  set(_crypto "${_fixture_dir}/libcrypto.a")
  file(WRITE "${_ssl}" "ssl")
  file(WRITE "${_crypto}" "crypto")
  set(_manifest "${_fixture_dir}/manifest.json")
  file(WRITE "${_manifest}"
    "{\"provider\":\"OPENSSL\",\"requestedLinkage\":\"${linkage}\","
    "\"dependencyRoot\":\"${_fixture_dir}\","
    "\"sslLinkInput\":\"${_ssl}\",\"cryptoLinkInput\":\"${_crypto}\"}\n")
  set(_static_map_argument "")
  if(static_map_mode MATCHES "^(GOOD|RELATIVE)$")
    set(_static_map "${_fixture_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "# Path: ${_artifact}\n"
      "# Arch: arm64\n"
      "# Object files:\n"
      "[  0] linker synthesized\n"
      "[  1] ${_ssl}(ssl_member.o)\n"
      "[  2] ${_crypto}(crypto_member.o)\n")
    if(static_map_mode STREQUAL "RELATIVE")
      file(RELATIVE_PATH _relative_artifact "${_fixture_dir}" "${_artifact}")
      file(READ "${_static_map}" _map_text)
      string(REPLACE "${_artifact}" "${_relative_artifact}" _map_text "${_map_text}")
      file(WRITE "${_static_map}" "${_map_text}")
    endif()
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  elseif(static_map_mode STREQUAL "SSL_ONLY")
    set(_static_map "${_fixture_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "# Path: ${_artifact}\n"
      "# Arch: arm64\n"
      "# Object files:\n"
      "[  0] linker synthesized\n"
      "[  1] ${_ssl}(ssl_member.o)\n")
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  elseif(static_map_mode STREQUAL "WRONG_OUTPUT")
    set(_other_artifact "${_fixture_dir}/another-driver.dylib")
    file(WRITE "${_other_artifact}" "other")
    set(_static_map "${_fixture_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "# Path: ${_other_artifact}\n"
      "# Arch: arm64\n"
      "# Object files:\n"
      "[  0] linker synthesized\n"
      "[  1] ${_ssl}(ssl_member.o)\n"
      "[  2] ${_crypto}(crypto_member.o)\n")
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  elseif(static_map_mode STREQUAL "NONEXISTENT_ARTIFACT")
    set(_static_map "${_fixture_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "# Path: ${_fixture_dir}/nonexistent.dylib\n"
      "# Arch: arm64\n"
      "# Object files:\n"
      "[  0] linker synthesized\n"
      "[  1] ${_ssl}(ssl_member.o)\n"
      "[  2] ${_crypto}(crypto_member.o)\n")
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  elseif(static_map_mode STREQUAL "FOREIGN_MAP")
    set(_foreign_dir "${BINARY_DIR}/crypto-origin-darwin-foreign")
    file(MAKE_DIRECTORY "${_foreign_dir}")
    set(_static_map "${_foreign_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "# Path: ${_artifact}\n"
      "# Arch: arm64\n"
      "# Object files:\n"
      "[  0] linker synthesized\n"
      "[  1] ${_ssl}(ssl_member.o)\n"
      "[  2] ${_crypto}(crypto_member.o)\n")
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  elseif(static_map_mode STREQUAL "NO_PATH")
    set(_static_map "${_fixture_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "# Arch: arm64\n"
      "# Object files:\n"
      "[  0] linker synthesized\n"
      "[  1] ${_ssl}(ssl_member.o)\n"
      "[  2] ${_crypto}(crypto_member.o)\n")
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  elseif(static_map_mode STREQUAL "STALE_MAP")
    set(_static_map "${_fixture_dir}/static-link.map")
    file(WRITE "${_static_map}"
      "# Path: ${_artifact}\n"
      "# Arch: arm64\n"
      "# Object files:\n"
      "[  0] linker synthesized\n"
      "[  1] ${_ssl}(ssl_member.o)\n"
      "[  2] ${_crypto}(crypto_member.o)\n")
    execute_process(
      COMMAND python3 -c "import os, time; t = time.time() - 100; os.utime('${_static_map}', (t, t))")
    set(_static_map_argument "-DSTATIC_LINK_MAP=${_static_map}")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DARTIFACT=${_artifact}
            -DEXPECTED_PROVIDER=OPENSSL
            -DEXPECTED_LINKAGE=${linkage}
            -DPLATFORM=Darwin
            "-DINSPECTION_OUTPUT=${output}"
            "-DRESOLUTION_OUTPUT=${resolution}"
            -DCONFIG_MANIFEST=${_manifest}
            ${_static_map_argument}
            "-DSTATIC_LINKER_IDENTITY=@(#)PROGRAM:ld PROJECT:ld-1167.5"
            -DEVIDENCE=${_fixture_dir}/evidence.json
            -P "${_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(expect_success AND NOT _result EQUAL 0)
    message(FATAL_ERROR "darwin ${name} unexpectedly failed: ${_stderr}${_stdout}")
  elseif(NOT expect_success AND _result EQUAL 0)
    message(FATAL_ERROR "darwin ${name} unexpectedly passed")
  endif()
  if(expect_success)
    file(READ "${_fixture_dir}/evidence.json" _evidence)
    string(JSON _schema GET "${_evidence}" schemaVersion)
    string(JSON _reported_provider GET "${_evidence}" provider)
    string(JSON _origin GET "${_evidence}" dependencyOriginVerified)
    string(JSON _inputs GET "${_evidence}" configuredLinkInputsVerified)
    string(JSON _map GET "${_evidence}" staticLinkMapVerified)
    string(JSON _members GET "${_evidence}" staticArchiveMemberEvidenceVerified)
    string(JSON _qualified GET "${_evidence}" qualificationClaimed)
    set(_claims_wrong false)
    if(linkage STREQUAL "BUNDLED_STATIC")
      if(_origin OR NOT _inputs OR NOT _map OR NOT _members)
        set(_claims_wrong true)
      endif()
    elseif(NOT _origin OR _inputs OR _map OR _members)
      set(_claims_wrong true)
    endif()
    if(NOT _schema EQUAL 2 OR _claims_wrong OR _qualified OR
       NOT _reported_provider STREQUAL "OPENSSL")
      message(FATAL_ERROR "darwin ${name} emitted incorrect origin evidence")
    endif()
  endif()
endfunction()

run_darwin_fixture(darwin_static_ok BUNDLED_STATIC
  "/usr/lib/libc++.1.dylib\n/usr/lib/libSystem.B.dylib" "" true GOOD)
run_darwin_fixture(darwin_static_missing_map BUNDLED_STATIC
  "/usr/lib/libc++.1.dylib\n/usr/lib/libSystem.B.dylib" "" false NONE)
run_darwin_fixture(darwin_static_incomplete_map BUNDLED_STATIC
  "/usr/lib/libc++.1.dylib\n/usr/lib/libSystem.B.dylib" "" false SSL_ONLY)
run_darwin_fixture(darwin_static_wrong_output BUNDLED_STATIC
  "/usr/lib/libc++.1.dylib\n/usr/lib/libSystem.B.dylib" "" false WRONG_OUTPUT)
run_darwin_fixture(darwin_static_nonexistent_artifact BUNDLED_STATIC
  "/usr/lib/libc++.1.dylib\n/usr/lib/libSystem.B.dylib" "" false NONEXISTENT_ARTIFACT)
run_darwin_fixture(darwin_static_foreign_map BUNDLED_STATIC
  "/usr/lib/libc++.1.dylib\n/usr/lib/libSystem.B.dylib" "" false FOREIGN_MAP)
run_darwin_fixture(darwin_static_no_path BUNDLED_STATIC
  "/usr/lib/libc++.1.dylib\n/usr/lib/libSystem.B.dylib" "" false NO_PATH)
run_darwin_fixture(darwin_static_stale_map BUNDLED_STATIC
  "/usr/lib/libc++.1.dylib\n/usr/lib/libSystem.B.dylib" "" false STALE_MAP)
run_darwin_fixture(darwin_static_relative BUNDLED_STATIC
  "/usr/lib/libc++.1.dylib\n/usr/lib/libSystem.B.dylib" "" true RELATIVE)
run_darwin_fixture(darwin_static_dynamic_leak BUNDLED_STATIC
  "/opt/homebrew/lib/libssl.dylib\n/usr/lib/libSystem.B.dylib" "" false GOOD)

set(_unsupported_fixture_dir "${BINARY_DIR}/crypto-origin-darwin-unsupported-linker")
file(MAKE_DIRECTORY "${_unsupported_fixture_dir}")
set(_unsupported_artifact "${_unsupported_fixture_dir}/libodbcpp.dylib")
file(WRITE "${_unsupported_artifact}" "fixture")
set(_unsupported_ssl "${_unsupported_fixture_dir}/libssl.a")
set(_unsupported_crypto "${_unsupported_fixture_dir}/libcrypto.a")
file(WRITE "${_unsupported_ssl}" "ssl")
file(WRITE "${_unsupported_crypto}" "crypto")
set(_unsupported_manifest "${_unsupported_fixture_dir}/manifest.json")
file(WRITE "${_unsupported_manifest}"
  "{\"provider\":\"OPENSSL\",\"requestedLinkage\":\"BUNDLED_STATIC\","
  "\"dependencyRoot\":\"${_unsupported_fixture_dir}\","
  "\"sslLinkInput\":\"${_unsupported_ssl}\",\"cryptoLinkInput\":\"${_unsupported_crypto}\"}\n")
set(_unsupported_map "${_unsupported_fixture_dir}/static-link.map")
file(WRITE "${_unsupported_map}"
  "# Path: ${_unsupported_artifact}\n# Arch: arm64\n# Object files:\n"
  "[  1] ${_unsupported_ssl}(ssl_member.o)\n[  2] ${_unsupported_crypto}(crypto_member.o)\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -DARTIFACT=${_unsupported_artifact}
          -DEXPECTED_PROVIDER=OPENSSL
          -DEXPECTED_LINKAGE=BUNDLED_STATIC
          -DPLATFORM=Darwin
          "-DINSPECTION_OUTPUT=/usr/lib/libSystem.B.dylib"
          -DCONFIG_MANIFEST=${_unsupported_manifest}
          -DSTATIC_LINK_MAP=${_unsupported_map}
          "-DSTATIC_LINKER_IDENTITY=lld 17.0"
          -P "${_script}"
  RESULT_VARIABLE _unsupported_linker_result)
if(_unsupported_linker_result EQUAL 0)
  message(FATAL_ERROR "Bundled-static origin accepted an unsupported linker identity")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -DARTIFACT=${_unsupported_artifact}
          -DEXPECTED_PROVIDER=OPENSSL
          -DEXPECTED_LINKAGE=BUNDLED_STATIC
          -DPLATFORM=Darwin
          "-DINSPECTION_OUTPUT=/usr/lib/libSystem.B.dylib"
          -DCONFIG_MANIFEST=${_unsupported_manifest}
          -DSTATIC_LINK_MAP=${_unsupported_map}
          "-DSTATIC_LINKER_IDENTITY=custom-ld (PROGRAM:ld compatible)"
          -P "${_script}"
  RESULT_VARIABLE _substring_linker_result)
if(_substring_linker_result EQUAL 0)
  message(FATAL_ERROR "Bundled-static origin accepted an unanchored linker identity substring")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -DARTIFACT=${_unsupported_artifact}
          -DEXPECTED_PROVIDER=OPENSSL
          -DEXPECTED_LINKAGE=BUNDLED_STATIC
          -DPLATFORM=Darwin
          "-DINSPECTION_OUTPUT=/usr/lib/libSystem.B.dylib"
          -DCONFIG_MANIFEST=${_unsupported_manifest}
          -DSTATIC_LINK_MAP=${_unsupported_map}
          "-DSTATIC_LINKER_IDENTITY=GNU ld (fixture) 2.42"
          -P "${_script}"
  RESULT_VARIABLE _darwin_gnu_mismatch)
if(_darwin_gnu_mismatch EQUAL 0)
  message(FATAL_ERROR "Bundled-static origin accepted GNU ld on Darwin")
endif()

set(_linux_mismatch_dir "${BINARY_DIR}/crypto-origin-linux-mismatch")
file(MAKE_DIRECTORY "${_linux_mismatch_dir}")
set(_linux_artifact "${_linux_mismatch_dir}/libodbcpp.so")
file(WRITE "${_linux_artifact}" "fixture")
set(_linux_ssl "${_linux_mismatch_dir}/libssl.a")
set(_linux_crypto "${_linux_mismatch_dir}/libcrypto.a")
file(WRITE "${_linux_ssl}" "ssl")
file(WRITE "${_linux_crypto}" "crypto")
set(_linux_manifest "${_linux_mismatch_dir}/manifest.json")
file(WRITE "${_linux_manifest}"
  "{\"provider\":\"OPENSSL\",\"requestedLinkage\":\"BUNDLED_STATIC\","
  "\"dependencyRoot\":\"${_linux_mismatch_dir}\","
  "\"sslLinkInput\":\"${_linux_ssl}\",\"cryptoLinkInput\":\"${_linux_crypto}\"}\n")
set(_linux_map "${_linux_mismatch_dir}/static-link.map")
file(WRITE "${_linux_map}"
  "# Path: ${_linux_artifact}\n# Arch: arm64\n# Object files:\n"
  "[  1] ${_linux_ssl}(ssl_member.o)\n[  2] ${_linux_crypto}(crypto_member.o)\n")

execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -DARTIFACT=${_linux_artifact}
          -DEXPECTED_PROVIDER=OPENSSL
          -DEXPECTED_LINKAGE=BUNDLED_STATIC
          -DPLATFORM=Linux
          "-DINSPECTION_OUTPUT=NEEDED libc.so.6"
          -DCONFIG_MANIFEST=${_linux_manifest}
          -DSTATIC_LINK_MAP=${_linux_map}
          "-DSTATIC_LINKER_IDENTITY=@(#)PROGRAM:ld PROJECT:ld-1167.5"
          -P "${_script}"
  RESULT_VARIABLE _linux_apple_mismatch)
if(_linux_apple_mismatch EQUAL 0)
  message(FATAL_ERROR "Bundled-static origin accepted Apple ld on Linux")
endif()
