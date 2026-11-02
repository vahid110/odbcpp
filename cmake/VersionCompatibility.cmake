include(CMakePackageConfigHelpers)
function(odbcpp_write_package_version output)
  write_basic_package_version_file("${output}"
    VERSION "${ODBCPP_VERSION_CORE}" COMPATIBILITY ExactVersion)
  # Numeric CMake versions cannot distinguish beta/RC builds. Require an exact
  # semantic-version acknowledgement rather than aliasing every 1.1.0 beta.
  if(ODBCPP_VERSION_SEMVER MATCHES "-")
    file(READ "${output}" _numeric_version_file)
    file(WRITE "${output}"
      "\nif(NOT ODBCPP_ACCEPT_PRERELEASE STREQUAL \"${ODBCPP_VERSION_SEMVER}\")\n  set(PACKAGE_VERSION_COMPATIBLE FALSE)\n  set(PACKAGE_VERSION_EXACT FALSE)\n  set(PACKAGE_VERSION_UNSUITABLE TRUE)\n  return()\nendif()\n${_numeric_version_file}")
  endif()
endfunction()
