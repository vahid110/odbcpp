# Future caller-owned installed tree checks; no API/body execution.
# Source-only candidate: actual invocation requires separate root review/admission.
if(NOT ENABLE_ACTUAL_LEGACY_CONSUMER_CHECKS)
  message(FATAL_ERROR "Actual installed consumer checks are explicitly disabled")
endif()
# Optional GNU grammar remains unchanged; Darwin requires this explicit fixed profile.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  if(NOT LEGACY_DARWIN_ARCH STREQUAL "arm64")
    message(FATAL_ERROR "Explicit selected Darwin arm64 profile required")
  endif()
elseif(DEFINED LEGACY_DARWIN_ARCH AND NOT LEGACY_DARWIN_ARCH STREQUAL "")
  message(FATAL_ERROR "Darwin profile on unselected host")
endif()
foreach(_arg IN ITEMS INSTALLED_PREFIX CONSUMER_SOURCE OUTPUT_ROOT GNU_INCLUDEDIR GNU_LIBDIR
    SOURCE_ROOT BUILD_ROOT ISOLATION_OWNED_PARENT SYSTEM_ROOTS TOOLCHAIN_ROOTS)
  if(NOT DEFINED ${_arg} OR "${${_arg}}" STREQUAL "")
    message(FATAL_ERROR "Missing explicit consumer input: ${_arg}")
  endif()
endforeach()
if(NOT IS_ABSOLUTE "${INSTALLED_PREFIX}" OR NOT IS_ABSOLUTE "${OUTPUT_ROOT}" OR
   EXISTS "${OUTPUT_ROOT}" OR IS_SYMLINK "${OUTPUT_ROOT}" OR
   NOT EXISTS "${CONSUMER_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "Fresh owned output and actual installed prefix required")
endif()
if(IS_ABSOLUTE "${GNU_INCLUDEDIR}" OR IS_ABSOLUTE "${GNU_LIBDIR}" OR
   GNU_INCLUDEDIR MATCHES "(^|/)\\.\\.(/|$)" OR GNU_LIBDIR MATCHES "(^|/)\\.\\.(/|$)")
  message(FATAL_ERROR "Only explicit relative GNU directory projection supported")
endif()
get_filename_component(_prefix_real "${INSTALLED_PREFIX}" REALPATH)
get_filename_component(_output_parent "${OUTPUT_ROOT}" DIRECTORY)
get_filename_component(_output_name "${OUTPUT_ROOT}" NAME)
file(REAL_PATH "${_output_parent}" _parent_identity)
set(_output_real "${_parent_identity}/${_output_name}")
if(NOT IS_DIRECTORY "${_output_parent}" OR _output_name STREQUAL ".." OR _output_name STREQUAL ".")
  message(FATAL_ERROR "Existing canonical output parent required")
endif()
file(RELATIVE_PATH _output_inside "${_prefix_real}" "${_output_real}")
if(IS_SYMLINK "${INSTALLED_PREFIX}" OR
   (NOT IS_ABSOLUTE "${_output_inside}" AND NOT _output_inside MATCHES "^\\.\\.(/|$)"))
  message(FATAL_ERROR "Output must not overlap installed prefix")
endif()
foreach(_bad IN ITEMS "${SOURCE_ROOT}" "${BUILD_ROOT}")
  file(REAL_PATH "${_bad}" _bad_real)
  file(RELATIVE_PATH _inside "${_bad_real}" "${_output_real}")
  if(NOT IS_ABSOLUTE "${_inside}" AND NOT _inside MATCHES "^\\.\\.(/|$)")
    message(FATAL_ERROR "Output overlaps forbidden source/build root")
  endif()
endforeach()
set(_members
  "core/database/backend_capabilities.h"
  "core/database/backend_provider.h"
  "core/database/backend_result.h"
  "core/database/catalog_execution.h"
  "core/database/catalog_queries.h"
  "core/database/catalog_request.h"
  "core/database/database_factory.h"
  "core/database/error_policy.h"
  "core/database/generic_database_connection.h"
  "core/database/i_database_connection.h"
  "core/database/i_protocol_parser.h"
  "core/database/mysql/authentication.h"
  "core/database/mysql/connection_security.h"
  "core/database/mysql/date_parameter_descriptor.h"
  "core/database/mysql/date_parameter_wire.h"
  "core/database/mysql/date_wire.h"
  "core/database/mysql/datetime_parameter_descriptor.h"
  "core/database/mysql/datetime_parameter_wire.h"
  "core/database/mysql/datetime_wire.h"
  "core/database/mysql/decimal_wire.h"
  "core/database/mysql/error_wire.h"
  "core/database/mysql/handshake_wire.h"
  "core/database/mysql/mysql_protocol_parser.h"
  "core/database/mysql/mysql_session.h"
  "core/database/mysql/parameter_receipt_shape.h"
  "core/database/mysql/prepared_wire.h"
  "core/database/mysql/query_wire.h"
  "core/database/mysql/time_wire.h"
  "core/database/mysql/tls_negotiation.h"
  "core/database/native_type_info.h"
  "core/database/native_type_resolution.h"
  "core/database/parsed_query_result.h"
  "core/database/postgres/pg_backend_provider.h"
  "core/database/postgres/pg_catalog_profile.h"
  "core/database/postgres/pg_command.h"
  "core/database/postgres/pg_database_connection.h"
  "core/database/postgres/pg_messages.h"
  "core/database/postgres/pg_protocol_parser.h"
  "core/database/postgres/pg_sql_dialect.h"
  "core/database/postgres/pg_value.h"
  "core/database/postgres/redshift_catalog_query.h"
  "core/database/postgres/redshift_foreign_key_contract.h"
  "core/database/postgres/redshift_primary_key_contract.h"
  "core/database/postgres/scram_sha256.h"
  "core/database/query_parameter.h"
  "core/database/query_result.h"
  "core/database/result_validation.h"
  "core/database/session_cancellation.h"
  "core/database/session_cancellation_wire.h"
  "core/database/session_health.h"
  "core/database/session_reset.h"
  "core/database/sql_dialect.h"
  "core/database/sql_translation.h"
  "core/database/statement_description.h"
  "core/database/statement_kind.h"
  "core/database/transaction.h"
  "core/database/transaction_session.h"
  "core/database/type_definition.h"
  "core/transport/async_tls_transport.h"
  "core/transport/async_transport.h"
  "core/transport/cancellation_wait.h"
  "core/transport/deadline_model.h"
  "core/transport/epoll_transport.h"
  "core/transport/i_transport.h"
  "core/transport/iocp_transport.h"
  "core/transport/socket_transport.h"
  "core/transport/socket_wait.h"
  "core/transport/start_tls_transport.h"
  "core/transport/thread_pool_transport.h"
  "core/transport/tls_configurable_transport.h"
  "core/transport/tls_transport.h"
  "core/transport/transport_factory.h"
  "core/transport/transport_options.h"
  "core/util/base64.h"
  "core/util/deadline.h"
  "core/util/driver_logging.h"
  "core/util/errors.h"
  "core/util/exception_adapter.h"
  "core/util/hex.h"
  "core/util/platform.h"
  "core/util/result.h"
  "core/util/utf8.h"
)
file(GLOB_RECURSE _actual RELATIVE "${INSTALLED_PREFIX}/${GNU_INCLUDEDIR}/odbcpp"
  "${INSTALLED_PREFIX}/${GNU_INCLUDEDIR}/odbcpp/core/*.h")
list(SORT _actual)
list(SORT _members)
if(NOT "${_actual}" STREQUAL "${_members}")
  message(FATAL_ERROR "Installed legacy membership differs from exact79 policy")
endif()
foreach(_header IN LISTS _members)
  if(IS_SYMLINK "${INSTALLED_PREFIX}/${GNU_INCLUDEDIR}/odbcpp/${_header}")
    message(FATAL_ERROR "Symlink member unsupported")
  endif()
endforeach()
# Broad installed membership, not merely the legacy core subset.
file(GLOB_RECURSE _all_headers RELATIVE "${INSTALLED_PREFIX}/${GNU_INCLUDEDIR}"
  "${INSTALLED_PREFIX}/${GNU_INCLUDEDIR}/*.h" "${INSTALLED_PREFIX}/${GNU_INCLUDEDIR}/*.hpp")
set(_expected_headers "")
foreach(_member IN LISTS _members)
  list(APPEND _expected_headers "odbcpp/${_member}")
endforeach()
list(SORT _all_headers)
list(SORT _expected_headers)
if(NOT "${_all_headers}" STREQUAL "${_expected_headers}")
  message(FATAL_ERROR "Additional installed SDK/auth/private headers")
endif()
file(GLOB _package_files RELATIVE "${INSTALLED_PREFIX}/${GNU_LIBDIR}/cmake/odbcpp"
  "${INSTALLED_PREFIX}/${GNU_LIBDIR}/cmake/odbcpp/*")
list(SORT _package_files)
set(_expected_package "odbcppConfig.cmake;odbcppConfigVersion.cmake;odbcppTargets-release.cmake;odbcppTargets.cmake")
if(NOT "${_package_files}" STREQUAL "${_expected_package}")
  message(FATAL_ERROR "Unselected package declaration inventory")
endif()
file(READ "${INSTALLED_PREFIX}/${GNU_LIBDIR}/cmake/odbcpp/odbcppTargets.cmake" _exports)
string(REGEX MATCHALL "add_library\\([^ ]+" _decls "${_exports}")
list(SORT _decls)
if(NOT "${_decls}" STREQUAL "add_library(odbcpp::odbcpp_core;add_library(odbcpp::odbcpp_driver")
  message(FATAL_ERROR "Export graph is not exactly core and driver")
endif()
foreach(_root IN LISTS SYSTEM_ROOTS TOOLCHAIN_ROOTS)
  file(REAL_PATH "${_root}" _real_root)
  if(_real_root STREQUAL "/" OR NOT IS_DIRECTORY "${_real_root}")
    message(FATAL_ERROR "Unbounded or missing system/toolchain root")
  endif()
endforeach()
file(REAL_PATH "${SOURCE_ROOT}" SOURCE_ROOT)
file(REAL_PATH "${BUILD_ROOT}" BUILD_ROOT)
file(REAL_PATH "${INSTALLED_PREFIX}" INSTALLED_PREFIX)
set(FORBIDDEN_SOURCE_BUILD_ROOTS "${SOURCE_ROOT};${BUILD_ROOT}")
foreach(_env IN ITEMS CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH LIBRARY_PATH)
  if(NOT "$ENV{${_env}}" STREQUAL "")
    message(FATAL_ERROR "Ambient compiler input roots unsupported")
  endif()
endforeach()
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")
set(_fixture_source "${OUTPUT_ROOT}/consumer-source")
file(MAKE_DIRECTORY "${_fixture_source}")
foreach(_fixture IN ITEMS CMakeLists.txt contract_first_include.cpp driver_link_only.cpp mysql_first_include.cpp)
  file(COPY "${CONSUMER_SOURCE}/${_fixture}" DESTINATION "${_fixture_source}")
endforeach()
file(REAL_PATH "${_fixture_source}" CONSUMER_SOURCE)
function(_confined path prefix build source)
  if(NOT IS_ABSOLUTE "${path}" OR NOT EXISTS "${path}")
    message(FATAL_ERROR "Unresolved actual input path")
  endif()
  file(REAL_PATH "${path}" _identity)
  foreach(_bad IN LISTS FORBIDDEN_SOURCE_BUILD_ROOTS)
    file(RELATIVE_PATH _inside "${_bad}" "${_identity}")
    if(NOT IS_ABSOLUTE "${_inside}" AND NOT _inside MATCHES "^\\.\\.(/|$)")
      message(FATAL_ERROR "Actual input uses forbidden original root")
    endif()
  endforeach()
  set(_accepted FALSE)
  foreach(_root IN ITEMS "${prefix}" "${build}" "${source}")
    file(REAL_PATH "${_root}" _real_root)
    file(RELATIVE_PATH _inside "${_real_root}" "${_identity}")
    if(NOT IS_ABSOLUTE "${_inside}" AND NOT _inside MATCHES "^\\.\\.(/|$)")
      set(_accepted TRUE)
    endif()
  endforeach()
  foreach(_root IN LISTS SYSTEM_ROOTS TOOLCHAIN_ROOTS)
    file(REAL_PATH "${_root}" _real_root)
    file(RELATIVE_PATH _inside "${_real_root}" "${_identity}")
    if(NOT IS_ABSOLUTE "${_inside}" AND NOT _inside MATCHES "^\\.\\.(/|$)")
      set(_accepted TRUE)
    endif()
  endforeach()
  if(NOT _accepted)
    message(FATAL_ERROR "Actual input outside declared install/system/toolchain closure")
  endif()
endfunction()
function(_argv text prefix build source cwd expected_source phase)
  if(text MATCHES "[;`|&<>]")
    message(FATAL_ERROR "Unsupported shell/response-file grammar")
  endif()
  file(REAL_PATH "${cwd}" _cwd)
  file(REAL_PATH "${build}" _build)
  if(NOT _cwd STREQUAL _build)
    message(FATAL_ERROR "Recorded working directory mismatch")
  endif()
  separate_arguments(_tokens UNIX_COMMAND "${text}")
  list(LENGTH _tokens _count)
  if(_count LESS 2 OR _count GREATER 256)
    message(FATAL_ERROR "Actual argv budget")
  endif()
  set(_pending "")
  set(_first TRUE)
  set(_sources 0)
  set(_compile_switch FALSE)
  set(_arch_count 0)
  if(DEFINED LEGACY_DARWIN_ARCH AND NOT LEGACY_DARWIN_ARCH STREQUAL "arm64")
    message(FATAL_ERROR "Unselected Darwin architecture")
  endif()
  foreach(_token IN LISTS _tokens)
    if(_token MATCHES "^@")
      message(FATAL_ERROR "Unsupported shell/response-file grammar")
    endif()
    if(_first)
      if(NOT IS_ABSOLUTE "${_token}")
        message(FATAL_ERROR "Compiler executable not explicit")
      endif()
      _confined("${_token}" "${prefix}" "${build}" "${source}")
      file(REAL_PATH "${_token}" _tool)
      if(NOT _tool STREQUAL COMPILER_IDENTITY)
        message(FATAL_ERROR "Recorded compiler identity mismatch")
      endif()
      set(_first FALSE)
      continue()
    endif()
    if(_pending STREQUAL "architecture")
      if(NOT _token STREQUAL LEGACY_DARWIN_ARCH OR NOT _arch_count EQUAL 0)
        message(FATAL_ERROR "Actual architecture mismatch or duplicate")
      endif()
      math(EXPR _arch_count "${_arch_count}+1")
      set(_pending "")
      continue()
    endif()
    if(_pending)
      get_filename_component(_operand "${_token}" ABSOLUTE BASE_DIR "${_cwd}")
      if(_pending STREQUAL "output")
        get_filename_component(_parent "${_operand}" DIRECTORY)
        get_filename_component(_name "${_operand}" NAME)
        if(NOT IS_DIRECTORY "${_parent}" OR IS_SYMLINK "${_operand}")
          message(FATAL_ERROR "Output identity unresolved")
        endif()
        file(REAL_PATH "${_parent}" _parent)
        set(_operand "${_parent}/${_name}")
        file(RELATIVE_PATH _inside "${_build}" "${_operand}")
        if(IS_ABSOLUTE "${_inside}" OR _inside MATCHES "^\\.\\.(/|$)")
          message(FATAL_ERROR "Output escapes recorded build")
        endif()
      else()
        _confined("${_operand}" "${prefix}" "${build}" "${source}")
      endif()
      set(_pending "")
      continue()
    endif()
    if(_token STREQUAL "-arch")
      if(NOT LEGACY_DARWIN_ARCH STREQUAL "arm64")
        message(FATAL_ERROR "Darwin architecture mechanism unselected")
      endif()
      set(_pending architecture)
    elseif(_token STREQUAL "-c")
      set(_compile_switch TRUE)
    elseif(_token MATCHES "^(-o|-MF|-MT)$")
      set(_pending output)
    elseif(_token MATCHES "^(-I|-isystem|-iquote|-idirafter|-include|-imacros|-isysroot|--sysroot)$")
      set(_pending input)
    elseif(_token MATCHES "^-I(.+)$")
      get_filename_component(_operand "${CMAKE_MATCH_1}" ABSOLUTE BASE_DIR "${_cwd}")
      _confined("${_operand}" "${prefix}" "${build}" "${source}")
    elseif(_token MATCHES "^--sysroot=(.+)$")
      get_filename_component(_operand "${CMAKE_MATCH_1}" ABSOLUTE BASE_DIR "${_cwd}")
      _confined("${_operand}" "${prefix}" "${build}" "${source}")
    elseif(_token MATCHES "^-L" OR _token MATCHES "^-l")
      message(FATAL_ERROR "Unresolved library search input")
    elseif(_token STREQUAL "-Wl,-search_paths_first" OR _token STREQUAL "-Wl,-headerpad_max_install_names")
      if(NOT phase STREQUAL "link" OR NOT LEGACY_DARWIN_ARCH STREQUAL "arm64")
        message(FATAL_ERROR "Darwin linker mechanism unselected")
      endif()
    elseif(_token MATCHES "^-Wl,-rpath,(.+)$")
      if(NOT IS_ABSOLUTE "${CMAKE_MATCH_1}")
        message(FATAL_ERROR "Relative linker operand unsupported")
      endif()
      _confined("${CMAKE_MATCH_1}" "${prefix}" "${build}" "${source}")
    elseif(_token MATCHES "^(-c|-MD|-MMD|-MP|-fPIC|-fPIE|-pie|-shared|-rdynamic|-g|-O[0123s]|-Wall|-Wextra|-Wpedantic|-Werror|-Wno-unused-parameter|-Wno-unused-variable|-Wno-unused-function|-Wno-c\\+\\+98-compat|-std=(gnu\\+\\+20|c\\+\\+20)|-DNDEBUG|-DODBCPP_[A-Z_]+=[0124])$")
      # Closed non-input switches only.
    elseif(_token MATCHES "^-")
      message(FATAL_ERROR "Unknown actual argument mechanism")
    else()
      get_filename_component(_operand "${_token}" ABSOLUTE BASE_DIR "${_cwd}")
      _confined("${_operand}" "${prefix}" "${build}" "${source}")
      if(_token MATCHES "\\.(cpp|cc|cxx)$")
        file(REAL_PATH "${expected_source}" _expected)
        file(REAL_PATH "${_operand}" _actual)
        if(NOT phase STREQUAL "compile" OR NOT _actual STREQUAL _expected)
          message(FATAL_ERROR "Actual command source mismatch")
        endif()
        math(EXPR _sources "${_sources}+1")
      elseif(NOT _token MATCHES "\\.(o|a|so|dylib|tbd)(\\.[0-9.]+)?$")
        message(FATAL_ERROR "Unknown unresolved operand")
      endif()
    endif()
  endforeach()
  if(_pending OR (phase STREQUAL "compile" AND (NOT _sources EQUAL 1 OR NOT _compile_switch)) OR
      (phase STREQUAL "link" AND _compile_switch))
    message(FATAL_ERROR "Incomplete argv/source binding")
  endif()
  if(LEGACY_DARWIN_ARCH STREQUAL "arm64" AND NOT _arch_count EQUAL 1)
    message(FATAL_ERROR "Missing selected Darwin architecture")
  endif()
endfunction()

function(_consumer prefix label mode expected link)
  set(_build "${OUTPUT_ROOT}/${label}")
  set(_platform_args "")
  if(LEGACY_DARWIN_ARCH STREQUAL "arm64")
    list(APPEND _platform_args "-DCMAKE_OSX_ARCHITECTURES=${LEGACY_DARWIN_ARCH}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${_build}"
    "-DLEGACY_PREFIX=${prefix}" "-DLEGACY_INCLUDEDIR=${GNU_INCLUDEDIR}"
    "-DLEGACY_LIBDIR=${GNU_LIBDIR}" "-DLEGACY_MODE=${mode}"
    "-DLEGACY_FORBIDDEN_ROOTS=${FORBIDDEN_SOURCE_BUILD_ROOTS}"
    "-DLEGACY_SYSTEM_ROOTS=${SYSTEM_ROOTS}" "-DLEGACY_TOOLCHAIN_ROOTS=${TOOLCHAIN_ROOTS}"
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ${_platform_args}
    RESULT_VARIABLE _configure OUTPUT_FILE "${OUTPUT_ROOT}/${label}-configure.log"
    ERROR_FILE "${OUTPUT_ROOT}/${label}-configure-error.log")
  if(NOT _configure EQUAL 0)
    message(FATAL_ERROR "Consumer configure failed; not an expected header negative")
  endif()
  file(READ "${_build}/compiler-identity.txt" _compiler_path)
  string(STRIP "${_compiler_path}" _compiler_path)
  file(REAL_PATH "${_compiler_path}" COMPILER_IDENTITY)
  file(READ "${_build}/compile_commands.json" _commands)
  string(JSON _length LENGTH "${_commands}")
  math(EXPR _last "${_length}-1")
  foreach(_index RANGE 0 ${_last})
    string(JSON _command GET "${_commands}" ${_index} command)
    string(JSON _cwd GET "${_commands}" ${_index} directory)
    string(JSON _source GET "${_commands}" ${_index} file)
    get_filename_component(_source "${_source}" ABSOLUTE BASE_DIR "${_cwd}")
    file(REAL_PATH "${_source}" _source_real)
    if(NOT _source_real STREQUAL "${CONSUMER_SOURCE}/contract_first_include.cpp" AND
       NOT _source_real STREQUAL "${CONSUMER_SOURCE}/mysql_first_include.cpp" AND
       NOT _source_real STREQUAL "${CONSUMER_SOURCE}/driver_link_only.cpp")
      message(FATAL_ERROR "Compile record source outside approved fixture")
    endif()
    _argv("${_command}" "${prefix}" "${_build}" "${CONSUMER_SOURCE}" "${_cwd}" "${_source}" compile)
  endforeach()
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_build}" --target legacy_header
    RESULT_VARIABLE _compile OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
  file(WRITE "${OUTPUT_ROOT}/${label}-compile.log" "${_stdout}${_stderr}")
  if(expected)
    string(REPLACE "\\" "/" _diagnostic "${_stdout}${_stderr}")
    string(FIND "${_diagnostic}" "${expected}" _missing)
    if(_compile EQUAL 0 OR _missing EQUAL -1 OR NOT _diagnostic MATCHES
       "(file not found|No such file|Cannot open include file)")
      message(FATAL_ERROR "Negative fixture did not prove precise missing-header failure")
    endif()
  elseif(NOT _compile EQUAL 0)
    message(FATAL_ERROR "Positive declaration compile failed")
  endif()
  if(NOT expected)
    file(GLOB_RECURSE _deps "${_build}/CMakeFiles/legacy_header.dir/*.d")
    if(NOT _deps)
      message(FATAL_ERROR "Actual compiler depfile unavailable; no positive qualification")
    endif()
    foreach(_dep IN LISTS _deps)
      file(READ "${_dep}" _body)
      string(REPLACE "\\\n" " " _body "${_body}")
      string(REGEX REPLACE "^[^:]+:" "" _body "${_body}")
      separate_arguments(_inputs UNIX_COMMAND "${_body}")
      foreach(_input IN LISTS _inputs)
        _confined("${_input}" "${prefix}" "${_build}" "${CONSUMER_SOURCE}")
      endforeach()
    endforeach()
  endif()
  if(link)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_build}" --target legacy_driver_link_only
      RESULT_VARIABLE _link OUTPUT_FILE "${OUTPUT_ROOT}/${label}-link.log"
      ERROR_FILE "${OUTPUT_ROOT}/${label}-link-error.log")
    if(NOT EXISTS "${_build}/CMakeFiles/legacy_driver_link_only.dir/link.txt")
      message(FATAL_ERROR "Actual link command unavailable")
    endif()
    file(READ "${_build}/CMakeFiles/legacy_driver_link_only.dir/link.txt" _link_command)
    _argv("${_link_command}" "${prefix}" "${_build}" "${CONSUMER_SOURCE}" "${_build}" "" link)
    file(GLOB_RECURSE _link_deps "${_build}/CMakeFiles/legacy_driver_link_only.dir/*.d")
    if(NOT _link_deps)
      message(FATAL_ERROR "Address TU full dependency capture unavailable")
    endif()
    foreach(_dep IN LISTS _link_deps)
      file(READ "${_dep}" _body)
      string(REPLACE "\\\n" " " _body "${_body}")
      string(REGEX REPLACE "^[^:]+:" "" _body "${_body}")
      separate_arguments(_inputs UNIX_COMMAND "${_body}")
      foreach(_input IN LISTS _inputs)
        _confined("${_input}" "${prefix}" "${_build}" "${CONSUMER_SOURCE}")
      endforeach()
    endforeach()
    if(NOT _link EQUAL 0)
      message(FATAL_ERROR "Exported driver-address link failed")
    endif()
  endif()
endfunction()
if(EXPORT_ROOT_CORRECTED)
  set(_mode CORRECTED_ROOT_POSITIVE)
  set(_expected "")
  set(_link TRUE)
else()
  set(_mode CURRENT_ROOT_NEGATIVE)
  set(_expected "core/database/query_result.h")
  set(_link FALSE)
endif()
_consumer("${INSTALLED_PREFIX}" original "${_mode}" "${_expected}" "${_link}")
if(EXPORT_ROOT_CORRECTED)
  _consumer("${INSTALLED_PREFIX}" omitted-crypto MYSQL_CRYPTO_NEGATIVE "core/security/crypto.h" FALSE)
endif()
# Copy complete caller-installed prefix to a distinct space-containing location.
set(_relocated "${OUTPUT_ROOT}/relocated prefix")
file(MAKE_DIRECTORY "${_relocated}")
file(COPY "${INSTALLED_PREFIX}/" DESTINATION "${_relocated}")
# Only explicitly marked owned copies may be reversibly renamed. Never arbitrary sources.
set(_isolation_origins "${SOURCE_ROOT};${BUILD_ROOT};${INSTALLED_PREFIX}")
set(_restores "")
file(REAL_PATH "${ISOLATION_OWNED_PARENT}" _owner_parent)
foreach(_origin IN LISTS _isolation_origins)
  file(REAL_PATH "${_origin}" _origin_real)
  file(RELATIVE_PATH _inside "${_owner_parent}" "${_origin_real}")
  if(IS_ABSOLUTE "${_inside}" OR _inside MATCHES "^\\.\\.(/|$)" OR _inside STREQUAL "" OR
      NOT EXISTS "${_origin}/.legacy-consumer-isolation-owner")
    message(FATAL_ERROR "Original root is not a caller-owned isolation copy")
  endif()
  file(READ "${_origin}/.legacy-consumer-isolation-owner" _marker)
  if(NOT _marker STREQUAL "PRIVATE_LEGACY_CONSUMER_ISOLATION_V1\n")
    message(FATAL_ERROR "Isolation ownership marker mismatch")
  endif()
  if(EXISTS "${_origin_real}.isolated" OR IS_SYMLINK "${_origin_real}.isolated")
    message(FATAL_ERROR "Isolation backup collision")
  endif()
  list(APPEND _restores "${_origin_real}")
endforeach()
set(_unique_restores "${_restores}")
list(REMOVE_DUPLICATES _unique_restores)
if(NOT "${_unique_restores}" STREQUAL "${_restores}")
  message(FATAL_ERROR "Isolation roots alias")
endif()
foreach(_a IN LISTS _restores)
  foreach(_b IN LISTS _restores)
    if(NOT _a STREQUAL _b)
      file(REAL_PATH "${_a}" _real_a)
      file(REAL_PATH "${_b}" _real_b)
      file(RELATIVE_PATH _nested "${_real_a}" "${_real_b}")
      if(NOT IS_ABSOLUTE "${_nested}" AND NOT _nested MATCHES "^\\.\\.(/|$)")
        message(FATAL_ERROR "Isolation roots overlap or alias")
      endif()
    endif()
  endforeach()
endforeach()
file(WRITE "${OUTPUT_ROOT}/isolation-ledger.txt" "planned=${_restores}\n")
foreach(_origin IN LISTS _restores)
  file(APPEND "${OUTPUT_ROOT}/isolation-ledger.txt" "rename_started=${_origin}|${_origin}.isolated\n")
  file(RENAME "${_origin}" "${_origin}.isolated")
  if(EXISTS "${_origin}" OR NOT IS_DIRECTORY "${_origin}.isolated")
    message(FATAL_ERROR "Observed isolation rename unresolved")
  endif()
  file(APPEND "${OUTPUT_ROOT}/isolation-ledger.txt" "renamed=${_origin}|${_origin}.isolated\n")
endforeach()
list(APPEND FORBIDDEN_SOURCE_BUILD_ROOTS "${INSTALLED_PREFIX}")
foreach(_origin IN LISTS _restores)
  list(APPEND FORBIDDEN_SOURCE_BUILD_ROOTS "${_origin}.isolated")
endforeach()
# Failure retains isolated owned copies; root recovery must restore before retry.

_consumer("${_relocated}" relocated "${_mode}" "${_expected}" "${_link}")
if(EXPORT_ROOT_CORRECTED)
  _consumer("${_relocated}" relocated-crypto MYSQL_CRYPTO_NEGATIVE "core/security/crypto.h" FALSE)
endif()
# Actual compiler/link properties/logs are observations, never loader/runtime proof.

foreach(_origin IN LISTS _restores)
  if(EXISTS "${_origin}" OR NOT EXISTS "${_origin}.isolated")
    message(FATAL_ERROR "Isolation restoration state unresolved")
  endif()
  file(APPEND "${OUTPUT_ROOT}/isolation-ledger.txt" "restore_started=${_origin}.isolated|${_origin}\n")
  file(RENAME "${_origin}.isolated" "${_origin}")
  if(NOT IS_DIRECTORY "${_origin}" OR EXISTS "${_origin}.isolated")
    message(FATAL_ERROR "Observed restore unresolved")
  endif()
  file(APPEND "${OUTPUT_ROOT}/isolation-ledger.txt" "restored=${_origin}\n")
endforeach()
