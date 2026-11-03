# Product libraries (ADR 0002). Each library is compiled once and linked through
# its declared dependencies. Until the files move under libs/, sources keep their
# current paths, compile against the staged runtime include surface, and pass the
# same internal-include check as the runtime partition. The graph check therefore
# judges link dependencies only: a library could still include, and call into,
# headers of a library it may not link until ADR 0002 step 7 gives each library
# its own include directory.
include("${CMAKE_CURRENT_LIST_DIR}/LibraryGraph.cmake")
find_package(Threads REQUIRED)

function(odbcpp_add_library name)
  cmake_parse_arguments(ARG "" "EXPORT_NAME" "DEPENDS;SYSTEM_INCLUDES" ${ARGN})
  if(NOT ARG_EXPORT_NAME)
    message(FATAL_ERROR "odbcpp_add_library(${name}) requires EXPORT_NAME")
  endif()
  odbcpp_library_sources(${name} _sources)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sources})
  odbcpp_check_internal_includes("${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/cmake/internal-headers/runtime.txt" ${_sources})
  add_library(${name} STATIC ${_sources})
  target_compile_features(${name} PRIVATE cxx_std_20)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME ${ARG_EXPORT_NAME})
  target_include_directories(${name} PRIVATE "${ODBCPP_INTERNAL_INCLUDE_runtime}")
  if(ARG_SYSTEM_INCLUDES)
    target_include_directories(${name} SYSTEM PRIVATE ${ARG_SYSTEM_INCLUDES})
  endif()
  target_link_libraries(${name} PRIVATE ${ARG_DEPENDS})
  apply_compiler_settings(${name})
  set_property(GLOBAL APPEND PROPERTY ODBCPP_LIBRARIES ${name})
endfunction()

# Export names stay private until the installable packages of ADR 0002 step 9.
odbcpp_add_library(odbcpp_base EXPORT_NAME base_private)
odbcpp_add_library(odbcpp_logging EXPORT_NAME logging_private
  DEPENDS Threads::Threads
  SYSTEM_INCLUDES "${SPDLOG_INCLUDE_DIR}")

# Every source in the library directories must belong to a library; a new file
# would otherwise silently drop out of the build.
file(GLOB _util_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/sdk/src/util/*.cpp")
get_property(_libraries GLOBAL PROPERTY ODBCPP_LIBRARIES)
set(_assigned "")
foreach(_library IN LISTS _libraries)
  odbcpp_library_sources(${_library} _sources)
  list(APPEND _assigned ${_sources})
endforeach()
foreach(_source IN LISTS _util_sources)
  if(NOT _source IN_LIST _assigned)
    message(FATAL_ERROR "${_source} belongs to no product library (cmake/DriverSources.cmake)")
  endif()
endforeach()

# The check leaves a record; remove an old one so only this configuration's run
# can satisfy the test that the check ran.
file(REMOVE "${CMAKE_BINARY_DIR}/library-graph-checked.txt")
cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL odbcpp_check_library_graph)

foreach(_artifact IN ITEMS ${PROJECT_NAME}_core ${PROJECT_NAME}_driver)
  target_link_libraries(${_artifact} PRIVATE odbcpp_logging odbcpp_base)
endforeach()
