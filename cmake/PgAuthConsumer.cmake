# Private backend handoff, separate from provider-free AuthCore and AWS providers.
# No installed consumer target or authentication header API.
set(_pg_auth_header "odbcpp/auth/pg_credential_consumer.h")
odbcpp_resolve_internal_header("${PROJECT_SOURCE_DIR}" "${_pg_auth_header}" _pg_auth_owner)
set(ODBCPP_PG_AUTH_CONSUMER_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/pg-auth-consumer-include")
file(REMOVE_RECURSE "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}")
configure_file("${PROJECT_SOURCE_DIR}/${_pg_auth_owner}"
  "${ODBCPP_PG_AUTH_CONSUMER_INCLUDE}/${_pg_auth_header}" COPYONLY)
# Implementation is compiled once per product artifact in its backend partition.
# This build-only forwarding target avoids a core/consumer archive cycle.
add_library(odbcpp_auth_pg_consumer INTERFACE)
target_link_libraries(odbcpp_auth_pg_consumer INTERFACE odbcpp::core)
