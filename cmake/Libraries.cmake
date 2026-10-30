# Product libraries (ADR 0002). Each library is compiled once and linked through
# its declared dependencies. Until the files move under libs/, sources keep their
# current paths and compile against the staged runtime include surface.

# The only allowed library dependencies; checked by odbcpp_check_library_graph.
set(ODBCPP_LIBRARY_GRAPH
  "odbcpp_base="
  "odbcpp_logging=odbcpp_base")

function(odbcpp_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS;SYSTEM_INCLUDES" ${ARGN})
  add_library(${name} STATIC ${ARG_SOURCES})
  target_compile_features(${name} PRIVATE cxx_std_20)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(${name} PRIVATE "${ODBCPP_INTERNAL_INCLUDE_runtime}")
  if(ARG_SYSTEM_INCLUDES)
    target_include_directories(${name} SYSTEM PRIVATE ${ARG_SYSTEM_INCLUDES})
  endif()
  target_link_libraries(${name} PRIVATE ${ARG_DEPENDS})
  apply_compiler_settings(${name})
  set_property(TARGET ${name} PROPERTY ODBCPP_LIBRARY_DEPENDS "${ARG_DEPENDS}")
  set_property(GLOBAL APPEND PROPERTY ODBCPP_LIBRARIES ${name})
endfunction()

# Fails configuration when a library is missing from the graph or declares a
# dependency the graph does not allow.
function(odbcpp_check_library_graph)
  get_property(_libraries GLOBAL PROPERTY ODBCPP_LIBRARIES)
  foreach(_library IN LISTS _libraries)
    set(_allowed "")
    set(_known FALSE)
    foreach(_entry IN LISTS ODBCPP_LIBRARY_GRAPH)
      string(REPLACE "=" ";" _pair "${_entry}")
      list(GET _pair 0 _name)
      if(_name STREQUAL _library)
        set(_known TRUE)
        list(LENGTH _pair _size)
        if(_size GREATER 1)
          list(GET _pair 1 _allowed)
          string(REPLACE "," ";" _allowed "${_allowed}")
        endif()
      endif()
    endforeach()
    if(NOT _known)
      message(FATAL_ERROR "Library ${_library} is not in ODBCPP_LIBRARY_GRAPH")
    endif()
    get_target_property(_depends ${_library} ODBCPP_LIBRARY_DEPENDS)
    foreach(_dependency IN LISTS _depends)
      if(_dependency MATCHES "^odbcpp_" AND NOT _dependency IN_LIST _allowed)
        message(FATAL_ERROR "Library ${_library} may not depend on ${_dependency}")
      endif()
    endforeach()
  endforeach()
endfunction()

odbcpp_add_library(odbcpp_base
  SOURCES
    "${PROJECT_SOURCE_DIR}/sdk/src/util/base64.cpp"
    "${PROJECT_SOURCE_DIR}/sdk/src/util/scaled_decimal.cpp"
    "${PROJECT_SOURCE_DIR}/sdk/src/util/utf8.cpp")

odbcpp_add_library(odbcpp_logging
  SOURCES "${PROJECT_SOURCE_DIR}/sdk/src/util/driver_logging.cpp"
  DEPENDS odbcpp_base
  SYSTEM_INCLUDES "${SPDLOG_INCLUDE_DIR}")

odbcpp_check_library_graph()

foreach(_artifact IN ITEMS ${PROJECT_NAME}_core ${PROJECT_NAME}_driver)
  target_link_libraries(${_artifact} PRIVATE odbcpp_logging odbcpp_base)
endforeach()
