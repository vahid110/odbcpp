# ---- Examples ----
file(GLOB EXAMPLE_SOURCES "examples/*.cpp")

foreach(example_file ${EXAMPLE_SOURCES})
  get_filename_component(example_name ${example_file} NAME_WE)
  
  add_executable(${example_name} ${example_file})
  target_link_libraries(${example_name} PRIVATE ${PROJECT_NAME}::core)
  if(example_name STREQUAL "connection_pool_example")
    target_link_libraries(${example_name} PRIVATE odbcpp_prototype_pool)
  endif()
  apply_compiler_settings(${example_name})
  
  # Set output directory
  set_target_properties(${example_name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
  )
endforeach()