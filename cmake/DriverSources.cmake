# One production source inventory, shared with isolated provider qualification.
function(odbcpp_collect_driver_sources output database)
  get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  set(TARGET_DATABASE "${database}")
# Base sources (always included)
file(GLOB CORE_BASE_SOURCES
  "${_root}/core/database/*.cpp"
  "${_root}/core/database/*.h"
  "${_root}/sdk/src/session/*.cpp"
  "${_root}/sdk/internal/odbcpp/session/*.h"
  "${_root}/sdk/include/odbcpp/database/*.h"
  "${_root}/sdk/include/odbcpp/transport/*.h"
  "${_root}/sdk/include/odbcpp/util/*.h"
  "${_root}/sdk/src/transport/*.cpp"
  "${_root}/sdk/internal/odbcpp/transport/*.h"
  "${_root}/sdk/src/security/*.cpp"
  "${_root}/sdk/internal/odbcpp/security/*.h"
  "${_root}/sdk/internal/odbcpp/util/*.h"
  "${_root}/sdk/src/util/*.cpp"
)

# Prototype pooling is test/example scaffolding, never a production artifact.
list(FILTER CORE_BASE_SOURCES EXCLUDE REGEX "/connection_pool\\.(cpp|h)$")
set(CORE_SOURCES ${CORE_BASE_SOURCES})
list(APPEND CORE_SOURCES "${_root}/product/compiled_backend.cpp")

# Add database-specific sources based on TARGET_DATABASE
if(TARGET_DATABASE STREQUAL "POSTGRESQL" OR TARGET_DATABASE STREQUAL "REDSHIFT")
  # Core database protocol
  file(GLOB DB_SOURCES "${_root}/core/database/postgres/*.cpp" "${_root}/core/database/postgres/*.h")
  list(APPEND CORE_SOURCES ${DB_SOURCES})
  
  # Database-specific ODBC layer
  if(TARGET_DATABASE STREQUAL "REDSHIFT")
    file(GLOB ODBC_DB_SOURCES "${_root}/odbc/redshift/*.cpp" "${_root}/odbc/redshift/*.h")
    list(APPEND CORE_SOURCES ${ODBC_DB_SOURCES})
  else()
    file(GLOB ODBC_DB_SOURCES "${_root}/odbc/postgresql/*.cpp" "${_root}/odbc/postgresql/*.h")
    list(APPEND CORE_SOURCES ${ODBC_DB_SOURCES})
  endif()
  
elseif(TARGET_DATABASE STREQUAL "MYSQL")
  file(GLOB DB_SOURCES "${_root}/core/database/mysql/*.cpp" "${_root}/core/database/mysql/*.h")
  file(GLOB ODBC_DB_SOURCES "${_root}/odbc/mysql/*.cpp" "${_root}/odbc/mysql/*.h")
  list(APPEND CORE_SOURCES ${DB_SOURCES} ${ODBC_DB_SOURCES})
  
elseif(TARGET_DATABASE STREQUAL "SQLSERVER")
  file(GLOB DB_SOURCES "${_root}/core/database/sqlserver/*.cpp" "${_root}/core/database/sqlserver/*.h")
  file(GLOB ODBC_DB_SOURCES "${_root}/odbc/sqlserver/*.cpp" "${_root}/odbc/sqlserver/*.h")
  list(APPEND CORE_SOURCES ${DB_SOURCES} ${ODBC_DB_SOURCES})
  
else()
  message(FATAL_ERROR "Unknown TARGET_DATABASE: ${TARGET_DATABASE}")
endif()

# Add async transport
list(APPEND CORE_SOURCES 
  "${_root}/sdk/internal/odbcpp/transport/async_transport.h"
  "${_root}/sdk/internal/odbcpp/transport/thread_pool_transport.h"
  "${_root}/sdk/src/transport/thread_pool_transport.cpp"
)

# Add ODBC layer
list(APPEND CORE_SOURCES
  "${_root}/odbc/odbc_types.h"
  "${_root}/odbc/odbc_handles.h"
  "${_root}/odbc/odbc_handles.cpp"
  "${_root}/odbc/odbc_api.h"
  "${_root}/odbc/c_api_guard.h"
  "${_root}/odbc/odbc_api.cpp"
  "${_root}/odbc/connection_string.h"
  "${_root}/odbc/connection_string.cpp"
  "${_root}/odbc/result_types.h"
  "${_root}/odbc/result_types.cpp"
  "${_root}/odbc/text_data_converter.h"
  "${_root}/odbc/text_data_converter.cpp"
  "${_root}/odbc/testing_hooks.h"
  "${_root}/odbc/unicode.h"
  "${_root}/odbc/unicode.cpp"
)

if(WIN32)
  list(APPEND CORE_SOURCES "${_root}/odbc/windows_registry.cpp" "${_root}/odbc/windows_registry.h")
endif()

  include("${_root}/cmake/PrototypePool.cmake")
  odbcpp_check_prototype_quarantine(${CORE_SOURCES})
  set(${output} "${CORE_SOURCES}" PARENT_SCOPE)
endfunction()
