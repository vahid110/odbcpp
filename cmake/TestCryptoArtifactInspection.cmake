if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "SOURCE_DIR and BINARY_DIR are required")
endif()

set(_artifact "${BINARY_DIR}/crypto-inspection-fixture.bin")
file(WRITE "${_artifact}" "fixture")
set(_script "${SOURCE_DIR}/cmake/InspectCryptoArtifact.cmake")

function(run_fixture name linkage output expect_success)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DARTIFACT=${_artifact}
            -DEXPECTED_PROVIDER=OPENSSL
            -DEXPECTED_LINKAGE=${linkage}
            -DPLATFORM=Linux
            "-DINSPECTION_OUTPUT=${output}"
            -P "${_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(expect_success AND NOT _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly failed: ${_stderr}${_stdout}")
  elseif(NOT expect_success AND _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly passed")
  endif()
endfunction()

run_fixture(shared_ok SYSTEM_SHARED
  "NEEDED libssl.so.3\nNEEDED libcrypto.so.3" true)
run_fixture(static_ok BUNDLED_STATIC "NEEDED libc.so.6" true)
run_fixture(shared_missing_crypto SYSTEM_SHARED "NEEDED libssl.so.3" false)
run_fixture(static_has_crypto BUNDLED_STATIC
  "NEEDED libssl.so.3\nNEEDED libcrypto.so.3" false)
