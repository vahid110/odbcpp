cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
  message(FATAL_ERROR "SOURCE_DIR and FIXTURE_DIR are required")
endif()
file(MAKE_DIRECTORY "${FIXTURE_DIR}")
set(_module "${SOURCE_DIR}/cmake/InternalTargets.cmake")
set(_root "${FIXTURE_DIR}/source")
set(_base
  "${_root}/core/transport/socket_transport.cpp"
  "${_root}/core/database/generic_database_connection.cpp"
  "${_root}/product/compiled_backend.cpp"
  "${_root}/odbc/odbc_api.cpp")
include("${_module}")
odbcpp_partition_driver_sources(probe "${_root}" ${_base}
  "${_root}/core/database/connection_pool.cpp"
  "${_root}/odbc/windows_registry.cpp"
  "${_root}/core/database/postgres/pg_protocol_parser.cpp"
  "${_root}/core/database/query_result.h"
  "${_root}/core/transport/socket_transport.cpp")
list(LENGTH probe_runtime _runtime_count)
list(LENGTH probe_backend _backend_count)
list(LENGTH probe_composition _composition_count)
list(LENGTH probe_odbc _odbc_count)
if(NOT _runtime_count EQUAL 1 OR NOT _backend_count EQUAL 2 OR
   NOT _composition_count EQUAL 2 OR NOT _odbc_count EQUAL 2)
  message(FATAL_ERROR "Partition ownership or duplicate normalization changed")
endif()
if(NOT "${_root}/core/database/generic_database_connection.cpp" IN_LIST probe_backend OR
   NOT "${_root}/core/database/connection_pool.cpp" IN_LIST probe_composition OR
   NOT "${_root}/odbc/windows_registry.cpp" IN_LIST probe_odbc)
  message(FATAL_ERROR "PostgreSQL-family, legacy pool or Windows ownership changed")
endif()
function(reject_fixture name sources expected)
  set(_script "${FIXTURE_DIR}/${name}.cmake")
  file(WRITE "${_script}" "cmake_minimum_required(VERSION 3.20)\ninclude(\"${_module}\")\nodbcpp_partition_driver_sources(probe \"${_root}\" \"${sources}\")\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_script}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(_result EQUAL 0 OR NOT "${_out}${_err}" MATCHES "${expected}")
    message(FATAL_ERROR "Invalid ${name} partition was not rejected precisely: ${_out}${_err}")
  endif()
endfunction()
reject_fixture(unknown "${_base};${_root}/core/database/new_session.cpp" "Unclassified internal source")
reject_fixture(escape "${_base};${_root}/../outside.cpp" "Unclassified internal source")
reject_fixture(resource "${_base};${_root}/odbc/setup.rc" "Unsupported internal source")
reject_fixture(empty "${_root}/core/transport/socket_transport.cpp" "Empty internal source partition: backend")
