# Private backend handoff, separate from provider-free AuthCore and AWS providers.
# Explicit consumer target only; no installed API or link into the ODBC driver.
set(_pg_auth_header "odbcpp/auth/pg_credential_consumer.h")
odbcpp_resolve_internal_header("${PROJECT_SOURCE_DIR}" "${_pg_auth_header}" _pg_auth_owner)
set(ODBCPP_PG_AUTH_CONSUMER_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/pg-auth-consumer-include")
file(REMOVE_RECURSE "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}")
configure_file("${PROJECT_SOURCE_DIR}/${_pg_auth_owner}"
  "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}/${_pg_auth_header}" COPYONLY)
add_library(odbcpp_auth_pg_consumer STATIC EXCLUDE_FROM_ALL
  "${PROJECT_SOURCE_DIR}/sdk/src/adapters/pg_credential_consumer.cpp")
target_compile_features(odbcpp_auth_pg_consumer PRIVATE cxx_std_20)
target_include_directories(odbcpp_auth_pg_consumer PRIVATE
  "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}" "${ODBCPP_INTERNAL_INCLUDE_backend}"
  "${ODBCPP_INTERNAL_INCLUDE_auth}")
target_link_libraries(odbcpp_auth_pg_consumer PRIVATE odbcpp::core odbcpp_auth_core)
apply_compiler_settings(odbcpp_auth_pg_consumer)
