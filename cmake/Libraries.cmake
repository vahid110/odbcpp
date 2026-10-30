# Product libraries (ADR 0002). Each library is compiled once and linked through
# its declared dependencies. Until the files move under libs/, sources keep their
# current paths, compile against the staged runtime include surface, and pass the
# same internal-include check as the runtime partition.
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

# Fails configuration on a malformed graph, a graph entry without a library, a
# library missing from the graph, or a link the graph does not allow. Runs at the
# end of configuration and reads each target's actual link properties, so links
# added anywhere in the build count.
function(odbcpp_check_library_graph)
  odbcpp_library_graph_problem("${ODBCPP_LIBRARY_GRAPH}" _problem)
  if(_problem)
    message(FATAL_ERROR "${_problem}")
  endif()
  get_property(_libraries GLOBAL PROPERTY ODBCPP_LIBRARIES)
  foreach(_entry IN LISTS ODBCPP_LIBRARY_GRAPH)
    string(REGEX REPLACE "=.*$" "" _name "${_entry}")
    if(NOT _name IN_LIST _libraries)
      message(FATAL_ERROR "Library graph entry without a library: ${_name}")
    endif()
  endforeach()
  foreach(_library IN LISTS _libraries)
    set(_links "")
    foreach(_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_values ${_library} ${_property})
      if(_values)
        list(APPEND _links ${_values})
      endif()
    endforeach()
    odbcpp_library_link_problem("${ODBCPP_LIBRARY_GRAPH}" ${_library} "${_links}" _problem)
    if(_problem)
      message(FATAL_ERROR "${_problem}")
    endif()
  endforeach()
endfunction()

# Export names stay private until the installable packages of ADR 0002 step 9.
odbcpp_add_library(odbcpp_base EXPORT_NAME base_private)
odbcpp_add_library(odbcpp_logging EXPORT_NAME logging_private
  DEPENDS Threads::Threads
  SYSTEM_INCLUDES "${SPDLOG_INCLUDE_DIR}")

cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL odbcpp_check_library_graph)

foreach(_artifact IN ITEMS ${PROJECT_NAME}_core ${PROJECT_NAME}_driver)
  target_link_libraries(${_artifact} PRIVATE odbcpp_logging odbcpp_base)
endforeach()
