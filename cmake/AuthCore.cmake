# Private, provider-free authentication archive. Installed only to close the
# static core dependency; authentication headers remain private.
find_package(Threads REQUIRED)
set(ODBCPP_PARTITION_auth
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/auth_core.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/response_operation.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/checked_response_boundary.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/checked_response_stream.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/temporary_db_validity.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/issuer_timestamp.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/aws_db_response_fields.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/bounded_response_stream.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/aws_db_json_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/checked_aws_db_json_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/aws_db_xml_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/checked_aws_db_xml_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/redshift_serverless_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/redshift_provisioned_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/src/auth/redshift_withiam_response.cpp")
odbcpp_stage_internal_includes(auth)
add_library(odbcpp_auth_core STATIC ${ODBCPP_PARTITION_auth})
target_compile_features(odbcpp_auth_core PRIVATE cxx_std_20)
target_link_libraries(odbcpp_auth_core PRIVATE Threads::Threads)
target_include_directories(odbcpp_auth_core PRIVATE "${ODBCPP_INTERNAL_INCLUDE_auth}")
set_target_properties(odbcpp_auth_core PROPERTIES EXPORT_NAME auth_core_private)
apply_compiler_settings(odbcpp_auth_core)
