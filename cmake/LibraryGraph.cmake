# Allowed dependencies between product libraries (ADR 0002). The rule functions
# are pure, so cmake/TestLibraryGraph.cmake exercises them in script mode; the
# target walk and the check itself need a project and are exercised by the
# scratch projects in that test.

# Each entry is "<library>=<allowed>[,<allowed>...]"; an empty list allows no
# product-library dependency.
set(ODBCPP_LIBRARY_GRAPH
  "odbcpp_base="
  "odbcpp_logging=odbcpp_base")

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

# Sets <out> to the first disallowed product-library dependency among <links>,
# or to "". Every odbcpp target or namespace name counts, wherever it appears
# in a link item, including inside any generator expression.
function(odbcpp_library_link_problem graph library links out)
  odbcpp_library_allowed("${graph}" "${library}" _allowed)
  if(_allowed STREQUAL "UNKNOWN")
    set(${out} "Library ${library} is not in the library graph" PARENT_SCOPE)
    return()
  endif()
  foreach(_link IN LISTS links)
    string(REGEX MATCHALL "odbcpp(_[A-Za-z0-9_]+|::[A-Za-z0-9_]+)" _names "${_link}")
    foreach(_name IN LISTS _names)
      if(NOT _name IN_LIST _allowed)
        set(${out} "Library ${library} may not depend on ${_name}" PARENT_SCOPE)
        return()
      endif()
    endforeach()
  endforeach()
  set(${out} "" PARENT_SCOPE)
endfunction()

# Properties through which a target can pull in another target's code.
set(ODBCPP_LIBRARY_LINK_PROPERTIES LINK_LIBRARIES INTERFACE_LINK_LIBRARIES
  INTERFACE_LINK_LIBRARIES_DIRECT LINK_OPTIONS INTERFACE_LINK_OPTIONS SOURCES
  INTERFACE_SOURCES MANUALLY_ADDED_DEPENDENCIES)

# Sets <out> to every target <target> depends on through any of those
# properties, aliases resolved and wrapper targets followed transitively, stopping
# at product libraries (whose own links the check judges separately). Names in
# generator expressions count when they name a target; plain paths never do.
function(odbcpp_library_dependencies target products out)
  set(_pending "${target}")
  set(_seen "")
  set(_found "")
  while(_pending)
    list(POP_FRONT _pending _current)
    foreach(_property IN LISTS ODBCPP_LIBRARY_LINK_PROPERTIES)
      get_target_property(_values ${_current} ${_property})
      if(NOT _values)
        continue()
      endif()
      foreach(_value IN LISTS _values)
        # Split only on generator-expression syntax, so a path stays whole and
        # cannot be mistaken for a target; "::" namespaces survive.
        string(REPLACE "::" "@NS@" _text "${_value}")
        string(REGEX REPLACE "[$<>:,]" ";" _text "${_text}")
        string(REPLACE "@NS@" "::" _tokens "${_text}")
        foreach(_token IN LISTS _tokens)
          if(NOT TARGET "${_token}")
            continue()
          endif()
          get_target_property(_aliased "${_token}" ALIASED_TARGET)
          if(_aliased)
            set(_token "${_aliased}")
          endif()
          if(_token STREQUAL target OR _token IN_LIST _seen)
            continue()
          endif()
          list(APPEND _seen "${_token}")
          list(APPEND _found "${_token}")
          if(NOT _token IN_LIST products)
            list(APPEND _pending "${_token}")
          endif()
        endforeach()
      endforeach()
    endforeach()
  endwhile()
  set(${out} "${_found}" PARENT_SCOPE)
endfunction()

# Fails configuration on a malformed graph, a graph entry without a library, a
# library missing from the graph, or a dependency the graph does not allow.
# Called deferred at the end of configuration, so dependencies added anywhere
# count. (A cmake_language(DEFER) queued after this one would run later and is
# not seen; the record below lets the tests prove the check ran.)
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
  set(_record "")
  foreach(_library IN LISTS _libraries)
    odbcpp_library_dependencies(${_library} "${_libraries}" _dependencies)
    set(_product_dependencies "")
    foreach(_dependency IN LISTS _dependencies)
      if(_dependency MATCHES "^odbcpp")
        list(APPEND _product_dependencies "${_dependency}")
      endif()
    endforeach()
    odbcpp_library_link_problem("${ODBCPP_LIBRARY_GRAPH}" ${_library} "${_product_dependencies}" _problem)
    if(_problem)
      message(FATAL_ERROR "${_problem}")
    endif()
    string(APPEND _record "${_library}: ${_dependencies}\n")
  endforeach()
  file(WRITE "${CMAKE_BINARY_DIR}/library-graph-checked.txt" "${_record}")
endfunction()
