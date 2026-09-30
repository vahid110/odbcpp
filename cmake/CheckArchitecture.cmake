if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB _shared_odbc
  "${SOURCE_DIR}/odbc/*.h"
  "${SOURCE_DIR}/odbc/*.cpp")
foreach(_file IN LISTS _shared_odbc)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ODBCPP_ENABLE_(POSTGRESQL|REDSHIFT|MYSQL|SQLSERVER)")
    message(FATAL_ERROR "Backend selection macro leaked into shared ODBC: ${_file}")
  endif()
  if(_contents MATCHES "core/database/(postgres|mysql|sqlserver)/" OR
     _contents MATCHES "(postgres|mysql|sqlserver)::")
    message(FATAL_ERROR "Concrete backend leaked into shared ODBC: ${_file}")
  endif()
  if(_contents MATCHES "#[ \t]*include[ \t]*[<\"]product/")
    message(FATAL_ERROR "Shared ODBC depends on product composition: ${_file}")
  endif()
endforeach()

file(GLOB _transport_files
  "${SOURCE_DIR}/core/transport/*.h"
  "${SOURCE_DIR}/core/transport/*.cpp")
foreach(_file IN LISTS _transport_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "#[ \t]*include[ \t]*[<\"].*database/")
    message(FATAL_ERROR "Transport depends on database layer: ${_file}")
  endif()
endforeach()

file(GLOB _database_root
  "${SOURCE_DIR}/core/database/*.h"
  "${SOURCE_DIR}/core/database/*.cpp")
foreach(_file IN LISTS _database_root)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "ODBCPP_ENABLE_(POSTGRESQL|REDSHIFT|MYSQL|SQLSERVER)")
    message(FATAL_ERROR "Backend selection outside product composition: ${_file}")
  endif()
  if(_contents MATCHES "(database/)?(postgres|mysql|sqlserver)/")
    message(FATAL_ERROR "Concrete backend include outside product composition: ${_file}")
  endif()
  if(_contents MATCHES "#[ \t]*include[ \t]*[<\"]product/")
    message(FATAL_ERROR "Core database source depends on product composition: ${_file}")
  endif()
endforeach()

file(GLOB_RECURSE _backend_files
  "${SOURCE_DIR}/core/database/postgres/*.h"
  "${SOURCE_DIR}/core/database/postgres/*.cpp"
  "${SOURCE_DIR}/core/database/mysql/*.h"
  "${SOURCE_DIR}/core/database/mysql/*.cpp"
  "${SOURCE_DIR}/core/database/sqlserver/*.h"
  "${SOURCE_DIR}/core/database/sqlserver/*.cpp")
foreach(_file IN LISTS _backend_files)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "#[ \t]*include[ \t]*[<\"]odbc/")
    message(FATAL_ERROR "Backend depends on ODBC adapter: ${_file}")
  endif()
  if(_contents MATCHES "#[ \t]*include[ \t]*[<\"]openssl/")
    message(FATAL_ERROR
      "Backend bypasses the private cryptography adapter: ${_file}")
  endif()
  if(_file MATCHES "/postgres/" AND
     _contents MATCHES "(mysql|sqlserver)/")
    message(FATAL_ERROR "PostgreSQL backend depends on a sibling: ${_file}")
  elseif(_file MATCHES "/mysql/" AND
         _contents MATCHES "(postgres|sqlserver)/")
    message(FATAL_ERROR "MySQL backend depends on a sibling: ${_file}")
  elseif(_file MATCHES "/sqlserver/" AND
         _contents MATCHES "(postgres|mysql)/")
    message(FATAL_ERROR "SQL Server backend depends on a sibling: ${_file}")
  endif()
endforeach()

file(GLOB _security_headers "${SOURCE_DIR}/core/security/*.h")
foreach(_file IN LISTS _security_headers)
  file(READ "${_file}" _contents)
  if(_contents MATCHES "#[ \t]*include[ \t]*[<\"](openssl|aws-lc|boringssl)/" OR
     _contents MATCHES "(^|[^A-Za-z0-9_])(SSL_CTX|SSL|X509|EVP_[A-Za-z0-9_]+)[ \t]*[*&]")
    message(FATAL_ERROR
      "Provider type leaked into private cryptography contract: ${_file}")
  endif()
endforeach()

file(GLOB_RECURSE _installed_core_headers "${SOURCE_DIR}/core/*.h")
foreach(_file IN LISTS _installed_core_headers)
  if(_file MATCHES "/core/security/" OR
     _file MATCHES "/(tls_io|tls_peer_identity)\\.h$")
    continue()
  endif()
  file(READ "${_file}" _contents)
  if(_contents MATCHES "#[ \t]*include[ \t]*[<\"](openssl|aws-lc|boringssl)/" OR
     _contents MATCHES "(^|[^A-Za-z0-9_])(SSL_CTX|SSL|X509|EVP_[A-Za-z0-9_]+)[ \t]*[*&]")
    message(FATAL_ERROR
      "Cryptography provider API leaked into an installed core header: ${_file}")
  endif()
endforeach()
