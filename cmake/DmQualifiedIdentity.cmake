# Explicit qualification evidence input. Never infer unixODBC patch identity
# from SQL_DM_VER, filename, package label, or the configure host's default.
if(NOT DEFINED ODBCPP_DM_QUALIFIED_UNIXODBC_RECEIPT)
  set(ODBCPP_DM_QUALIFIED_UNIXODBC_RECEIPT "" CACHE FILEPATH
      "Reviewed unixODBC 2.3.14 build/profile receipt enabling known DM expectations")
endif()
set(_dm_qualified_version "")
set(_dm_qualified_library_path "")
set(_dm_qualified_library_sha256 "")
set(_dm_qualified_platform "")
set(_dm_qualified_snapshot "")
foreach(_field pointer_bytes sqlwchar_bytes driver_sqlwchar_bytes odbc_version)
  set(_dm_qualified_${_field} 0)
endforeach()
if(ODBCPP_DM_QUALIFIED_UNIXODBC_RECEIPT)
  file(READ "${ODBCPP_DM_QUALIFIED_UNIXODBC_RECEIPT}" _receipt)
  foreach(_field schema_version family version library_path library_sha256 platform
                 pointer_bytes sqlwchar_bytes driver_sqlwchar_bytes odbc_version evidence)
    string(JSON _dm_qualified_${_field} ERROR_VARIABLE _error GET "${_receipt}" "${_field}")
    if(_error)
      message(FATAL_ERROR "Manager qualification receipt missing/invalid ${_field}")
    endif()
  endforeach()
  if(NOT _dm_qualified_schema_version STREQUAL "1" OR
     NOT _dm_qualified_family STREQUAL "unixODBC" OR
     NOT _dm_qualified_version STREQUAL "2.3.14" OR
     NOT _dm_qualified_platform STREQUAL CMAKE_SYSTEM_NAME OR
     NOT CMAKE_SYSTEM_NAME MATCHES "^(Darwin|Linux)$" OR
     NOT _dm_qualified_pointer_bytes STREQUAL "8" OR
     NOT CMAKE_SIZEOF_VOID_P STREQUAL "8" OR
     NOT _dm_qualified_sqlwchar_bytes STREQUAL "2" OR
     NOT ODBCPP_EXPECT_DM_SQLWCHAR_SIZE STREQUAL "2" OR
     NOT _dm_qualified_driver_sqlwchar_bytes STREQUAL "2" OR
     NOT ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE STREQUAL "2" OR
     NOT _dm_qualified_odbc_version STREQUAL "3" OR
     NOT ODBC_DRIVER_MANAGER_FLAVOR STREQUAL "UNIXODBC" OR
     _dm_qualified_evidence STREQUAL "")
    message(FATAL_ERROR "Manager qualification receipt profile does not match this build")
  endif()
  string(LENGTH "${_dm_qualified_library_sha256}" _hash_length)
  if(NOT _hash_length EQUAL 64 OR NOT _dm_qualified_library_sha256 MATCHES "^[0-9a-f]+$" OR
     NOT IS_ABSOLUTE "${_dm_qualified_library_path}" OR
     NOT EXISTS "${_dm_qualified_library_path}" OR NOT EXISTS "${ODBC_DRIVER_MANAGER_LIBRARY}")
    message(FATAL_ERROR "Manager qualification receipt requires an exact library SHA256 and absolute existing path")
  endif()
  file(REAL_PATH "${_dm_qualified_library_path}" _dm_qualified_library_path)
  file(REAL_PATH "${ODBC_DRIVER_MANAGER_LIBRARY}" _selected_manager)
  file(SHA256 "${_selected_manager}" _selected_hash)
  if(NOT _selected_manager STREQUAL _dm_qualified_library_path OR
     NOT _selected_hash STREQUAL _dm_qualified_library_sha256)
    message(FATAL_ERROR "Selected manager does not match the qualified build fingerprint")
  endif()
  set(_dm_qualified_snapshot "${CMAKE_CURRENT_BINARY_DIR}/dm-identity/qualified-manager.bin")
  configure_file("${_selected_manager}" "${_dm_qualified_snapshot}" COPYONLY)
  file(SHA256 "${_dm_qualified_snapshot}" _snapshot_hash)
  if(NOT _snapshot_hash STREQUAL _dm_qualified_library_sha256)
    message(FATAL_ERROR "Manager qualification snapshot does not match reviewed fingerprint")
  endif()
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/DmQualifiedIdentity.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/dm-identity/dm_qualified_identity.h" @ONLY)
