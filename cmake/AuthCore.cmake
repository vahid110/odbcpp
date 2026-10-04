# Private, provider-free authentication module. It is not linked into the driver
# or installed until real issuer integrations establish the public API boundary.
set(ODBCPP_PARTITION_auth
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/auth_core.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/temporary_db_validity.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/issuer_timestamp.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/aws_db_response_fields.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/bounded_response_stream.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/aws_db_json_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/aws_db_xml_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/redshift_serverless_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/redshift_provisioned_response.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/core/auth/redshift_withiam_response.cpp")
odbcpp_stage_internal_includes(auth)
add_library(odbcpp_auth_core STATIC ${ODBCPP_PARTITION_auth})
target_compile_features(odbcpp_auth_core PRIVATE cxx_std_20)
target_include_directories(odbcpp_auth_core PRIVATE "${ODBCPP_INTERNAL_INCLUDE_auth}")
apply_compiler_settings(odbcpp_auth_core)
