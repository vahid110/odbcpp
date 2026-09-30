if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "SOURCE_DIR and BINARY_DIR are required")
endif()
set(_script "${SOURCE_DIR}/cmake/CheckDriverExports.cmake")
set(_expected "${BINARY_DIR}/driver-export-fixture.txt")
file(WRITE "${_expected}" "SQLAllocHandle\nSQLConnect\n")

function(run_fixture name platform output expected_error)
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DEXPECTED=${_expected}
            -DPLATFORM=${platform}
            "-DINSPECTION_OUTPUT=${output}"
            -P "${_script}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
  if(NOT expected_error AND NOT _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly failed: ${_stderr}${_stdout}")
  elseif(expected_error AND _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly passed")
  elseif(expected_error AND NOT "${_stderr}${_stdout}" MATCHES "${expected_error}")
    message(FATAL_ERROR "${name} failed for the wrong reason: ${_stderr}${_stdout}")
  endif()
endfunction()

run_fixture(exact_surface Darwin
  "0001 T _SQLConnect\n0002 T _SQLAllocHandle" "")
run_fixture(unexpected_private_symbol Darwin
  "0001 T _SQLConnect\n0002 T _SQLAllocHandle\n0003 T __ZN6odbcpp7PrivateEv"
  "Unexpected: .*PrivateEv")
run_fixture(missing_odbc_export Darwin
  "0001 T _SQLConnect" "Missing: SQLAllocHandle")
run_fixture(windows_exact_surface Windows
  "    ordinal hint RVA      name\n          1    0 00001000 SQLAllocHandle\n          2    1 00002000 SQLConnect"
  "")
run_fixture(windows_forwarder_rejected Windows
  "          1    0 00001000 SQLAllocHandle = OTHER.SQLAllocHandle\n          2    1 00002000 SQLConnect"
  "Forwarded or aliased driver export is not allowed")
