if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "SOURCE_DIR and BINARY_DIR are required")
endif()

file(REMOVE_RECURSE "${BINARY_DIR}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${BINARY_DIR}"
          -DTARGET_DATABASE=MYSQL -DBUILD_TESTING=OFF
          -DODBCPP_CRYPTO_PROVIDER=${CRYPTO_PROVIDER}
          -DODBCPP_CRYPTO_LINKAGE=${CRYPTO_LINKAGE}
          -DODBCPP_CRYPTO_ROOT=${CRYPTO_ROOT}
  RESULT_VARIABLE _result
  OUTPUT_VARIABLE _stdout
  ERROR_VARIABLE _stderr)
if(_result EQUAL 0)
  message(FATAL_ERROR "Unimplemented MYSQL product configured successfully")
endif()
set(_output "${_stdout}\n${_stderr}")
if(NOT _output MATCHES "TARGET_DATABASE=MYSQL is not implemented")
  message(FATAL_ERROR
    "MYSQL configuration failed for an unexpected reason:\n${_output}")
endif()
