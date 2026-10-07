# ---- Installation Configuration ----
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Install the library
install(TARGETS ${PROJECT_NAME}_core ${PROJECT_NAME}_driver odbcpp_auth_core
  EXPORT ${PROJECT_NAME}Targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# A private application prefix needs the loadable driver, not SDK archives or
# headers. Keep this opt-in component out of the unchanged default SDK install.
install(TARGETS ${PROJECT_NAME}_driver
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT ODBCPPDriverRuntime EXCLUDE_FROM_ALL
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT ODBCPPDriverRuntime EXCLUDE_FROM_ALL
)

# Preserve the exact historical header membership and destination spelling.
# Python is a build preparation tool only; no installed target depends on it.
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(_legacy_manifest "${CMAKE_CURRENT_SOURCE_DIR}/sdk/legacy_install_headers.txt")
set(_legacy_owners_manifest "${CMAKE_CURRENT_SOURCE_DIR}/sdk/legacy_header_owners.txt")
set(_legacy_helper "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/sdk_legacy_header_projection.py")
set(_legacy_launcher "${CMAKE_CURRENT_SOURCE_DIR}/tools/ci/stage_sdk_legacy_headers.py")
set(_legacy_headers "${CMAKE_CURRENT_BINARY_DIR}/legacy-install-headers")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_legacy_manifest}" "${_legacy_owners_manifest}" "${_legacy_helper}" "${_legacy_launcher}")
file(STRINGS "${_legacy_owners_manifest}" _legacy_source_owners)
foreach(_owner IN LISTS _legacy_source_owners)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_owner}")
endforeach()
file(STRINGS "${_legacy_manifest}" _legacy_members)
execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${_legacy_launcher}"
  --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
  --manifest "${_legacy_manifest}" --owners-manifest "${_legacy_owners_manifest}"
  --output "${_legacy_headers}"
  RESULT_VARIABLE _legacy_result OUTPUT_VARIABLE _legacy_output ERROR_VARIABLE _legacy_error)
if(NOT _legacy_result EQUAL 0)
  message(FATAL_ERROR "Legacy declaration preparation failed: ${_legacy_output}${_legacy_error}")
endif()
foreach(_member IN LISTS _legacy_members)
  get_filename_component(_directory "${_member}" DIRECTORY)
  install(FILES "${_legacy_headers}/${_member}"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}/${_directory}")
endforeach()

# Install examples (optional)
if(BUILD_EXAMPLES)
  install(DIRECTORY examples/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples
      FILES_MATCHING PATTERN "*.cpp"
    PATTERN "connection_pool_example.cpp" EXCLUDE
  )
endif()

# Create and install package config files
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${PROJECT_NAME}Config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-manifest.json"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME})

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/odbcpp-crypto-manifest.json"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}
  COMPONENT ODBCPPDriverRuntime EXCLUDE_FROM_ALL)

install(EXPORT ${PROJECT_NAME}Targets
  FILE ${PROJECT_NAME}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

if(WIN32)
  install(TARGETS odbcpp_setup RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
