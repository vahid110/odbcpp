# Allowed dependencies between product libraries (ADR 0002). Pure functions so
# cmake/TestLibraryGraph.cmake can exercise them in script mode.

# Each entry is "<library>=<allowed>[,<allowed>...]"; an empty list allows no
# product-library dependency.
set(ODBCPP_LIBRARY_GRAPH
  "odbcpp_base="
  "odbcpp_logging=")

# Sets <out> to the allowed dependencies of <library>, or to "UNKNOWN".
function(odbcpp_library_allowed graph library out)
  set(_result "UNKNOWN")
  foreach(_entry IN LISTS graph)
    string(FIND "${_entry}" "=" _split)
    string(SUBSTRING "${_entry}" 0 ${_split} _name)
    if(_name STREQUAL library)
      math(EXPR _start "${_split} + 1")
      string(SUBSTRING "${_entry}" ${_start} -1 _allowed)
      string(REPLACE "," ";" _result "${_allowed}")
    endif()
  endforeach()
  set(${out} "${_result}" PARENT_SCOPE)
endfunction()

# Sets <out> to the first problem with the graph itself, or to "".
function(odbcpp_library_graph_problem graph out)
  set(_names "")
  foreach(_entry IN LISTS graph)
    if(NOT _entry MATCHES "^(odbcpp_[a-z_]+)=(odbcpp_[a-z_]+(,odbcpp_[a-z_]+)*)?$")
      set(${out} "Malformed library graph entry: ${_entry}" PARENT_SCOPE)
      return()
    endif()
    if(CMAKE_MATCH_1 IN_LIST _names)
      set(${out} "Duplicate library graph entry: ${CMAKE_MATCH_1}" PARENT_SCOPE)
      return()
    endif()
    list(APPEND _names "${CMAKE_MATCH_1}")
  endforeach()
  set(${out} "" PARENT_SCOPE)
endfunction()

# Sets <out> to the first disallowed product-library dependency among <links>
# (a target's LINK_LIBRARIES), or to "". Any link item naming an odbcpp target
# or namespace counts, including generator-expression wrappers.
function(odbcpp_library_link_problem graph library links out)
  odbcpp_library_allowed("${graph}" "${library}" _allowed)
  if(_allowed STREQUAL "UNKNOWN")
    set(${out} "Library ${library} is not in the library graph" PARENT_SCOPE)
    return()
  endif()
  foreach(_link IN LISTS links)
    string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _name "${_link}")
    if(_name MATCHES "^odbcpp(_|::)" AND NOT _name IN_LIST _allowed)
      set(${out} "Library ${library} may not depend on ${_name}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} "" PARENT_SCOPE)
endfunction()
