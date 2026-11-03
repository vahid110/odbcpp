# Allowed dependencies between product libraries (ADR 0002). The rule functions
# are pure, so cmake/TestLibraryGraph.cmake exercises them in script mode; the
# target walk and the check itself need a project and are exercised by the
# scratch projects in that test.
#
# Include-level layering waits for ADR 0002 step 7, when each library gets its
# own include directory; until then, the check judges link dependencies only.

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
  INTERFACE_SOURCES MANUALLY_ADDED_DEPENDENCIES STATIC_LIBRARY_OPTIONS
  LINK_DEPENDS INTERFACE_LINK_DEPENDS)

# Each dependency entry of a target this build defines must be a plain name or
# path, or allowed generator expressions around plain text, optionally nested
# as CMake stores for PRIVATE links on static product libraries
# ($<LINK_ONLY:$<BUILD_INTERFACE:x>> or $<LINK_ONLY:$<$<CONFIG:...>:x>>).
# Anything else could assemble a target name only at generate time, which the
# walk below cannot see.
set(ODBCPP_LIBRARY_ALLOWED_GENEX LINK_ONLY BUILD_INTERFACE INSTALL_INTERFACE CONFIG
  TARGET_OBJECTS TARGET_FILE TARGET_LINKER_FILE 0 1)
# Imported targets the product libraries may use; their own properties come
# from the system and are not judged by the strict generator-expression form.
# However, their link properties are still tokenized to ensure they do not depend
# on non-imported product targets. Any other imported target is refused.
set(ODBCPP_LIBRARY_ALLOWED_IMPORTED Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

# Sets <out> to the problem with one dependency entry <value>, or to "".
function(odbcpp_library_genex_problem value out)
  string(FIND "${value}" "$<" _genex)
  if(_genex EQUAL -1)
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  set(_payload "[^$<>]*")
  set(_head "")
  if(value MATCHES "^\\$<([A-Z_01]+):${_payload}>$")
    set(_head "${CMAKE_MATCH_1}")
  elseif(value MATCHES "^\\$<\\$<CONFIG:[A-Za-z0-9_,]*>:${_payload}>$")
    set(_head "CONFIG")
  elseif(value MATCHES "^\\$<LINK_ONLY:\\$<([A-Z_01]+):${_payload}>>$")
    set(_head "${CMAKE_MATCH_1}")
  elseif(value MATCHES "^\\$<LINK_ONLY:\\$<\\$<CONFIG:[A-Za-z0-9_,]*>:${_payload}>>$")
    set(_head "CONFIG")
  elseif(value MATCHES "^\\$<LINK_ONLY:\\$<BUILD_INTERFACE:\\$<\\$<CONFIG:[A-Za-z0-9_,]*>:${_payload}>>>$")
    set(_head "CONFIG")
  elseif(value MATCHES "^\\$<LINK_ONLY:\\$<\\$<CONFIG:[A-Za-z0-9_,]*>:\\$<BUILD_INTERFACE:${_payload}>>>$")
    set(_head "CONFIG")
  elseif(value MATCHES "^\\$<BUILD_INTERFACE:\\$<\\$<CONFIG:[A-Za-z0-9_,]*>:${_payload}>>$")
    set(_head "CONFIG")
  elseif(value MATCHES "^\\$<\\$<CONFIG:[A-Za-z0-9_,]*>:\\$<BUILD_INTERFACE:${_payload}>>$")
    set(_head "CONFIG")
  else()
    set(${out} "Dependency entry must be a plain name or one allowed generator expression around plain text: ${value}" PARENT_SCOPE)
    return()
  endif()
  if(NOT _head IN_LIST ODBCPP_LIBRARY_ALLOWED_GENEX)
    set(${out} "Generator expression $<${_head}:...> is not allowed in a product library dependency: ${value}" PARENT_SCOPE)
    return()
  endif()
  set(${out} "" PARENT_SCOPE)
endfunction()

# Sets <out> to every target <target> depends on through any of those
# properties, aliases resolved and wrapper targets followed transitively, stopping
# at product libraries (whose own links the check judges separately). Names in
# generator expressions count when they name a target; plain paths never do.
# Sets <problem> to a disallowed generator expression found on the way, or "".
function(odbcpp_library_dependencies target products out problem)
  set(_pending "${target}")
  set(_seen "")
  set(_found "")
  set(${problem} "" PARENT_SCOPE)
  while(_pending)
    list(POP_FRONT _pending _current)
    get_target_property(_imported ${_current} IMPORTED)
    if(_imported)
      if(NOT _current IN_LIST ODBCPP_LIBRARY_ALLOWED_IMPORTED)
        set(${problem} "Imported target ${_current} is not allowed in a product library dependency" PARENT_SCOPE)
        return()
      endif()
      foreach(_property IN LISTS ODBCPP_LIBRARY_LINK_PROPERTIES)
        get_target_property(_values ${_current} ${_property})
        if(NOT _values)
          continue()
        endif()
        foreach(_value IN LISTS _values)
          # Any reference to a product target (direct, namespaced, or assembled)
          # inside an allowed imported target's properties is forbidden.
          if(_value MATCHES "odbcpp")
            set(${problem} "Imported target ${_current} may not depend on product target: ${_value}" PARENT_SCOPE)
            return()
          endif()
          # Dynamic target generator expressions (e.g. TARGET_PROPERTY, GENEX_EVAL, TARGET_NAME)
          # can evaluate to arbitrary targets at generate time and are forbidden on imported targets.
          if(_value MATCHES "TARGET_|GENEX_EVAL")
            set(${problem} "Imported target ${_current} may not use dynamic generator expressions: ${_value}" PARENT_SCOPE)
            return()
          endif()
          # Multi-part or assembled generator expressions (e.g. $<1:odb><1:cpp_tls>,
          # $<1:odb>$<1:cpp_tls>, odb$<1:cpp_tls>, or $<1:odb>_tls) evade tokenization
          # and are forbidden on imported targets.
          if(_value MATCHES "\\$<" AND (NOT _value MATCHES "^\\$<" OR NOT _value MATCHES ">$" OR _value MATCHES ">[^;]*<"))
            set(${problem} "Imported target ${_current} may not use concatenated generator expressions: ${_value}" PARENT_SCOPE)
            return()
          endif()
          # Split on generator-expression syntax and whitespace, so paths stay whole
          # but space-separated lists are tokenized; "::" namespaces survive.
          string(REPLACE "::" "@NS@" _text "${_value}")
          string(REGEX REPLACE "[$<>:, \t\r\n]+" ";" _text "${_text}")
          string(REPLACE "@NS@" "::" _tokens "${_text}")
          foreach(_token IN LISTS _tokens)
            if(NOT TARGET "${_token}")
              continue()
            endif()
            get_target_property(_aliased "${_token}" ALIASED_TARGET)
            if(_aliased)
              set(_token "${_aliased}")
            endif()
            get_target_property(_tok_imported "${_token}" IMPORTED)
            if(NOT _tok_imported)
              set(${problem} "Imported target ${_current} may not depend on target ${_token}" PARENT_SCOPE)
              return()
            endif()
            if(NOT _token IN_LIST ODBCPP_LIBRARY_ALLOWED_IMPORTED)
              set(${problem} "Imported target ${_current} may not depend on imported target ${_token}" PARENT_SCOPE)
              return()
            endif()
            if(NOT _token IN_LIST _seen)
              list(APPEND _seen "${_token}")
              list(APPEND _pending "${_token}")
            endif()
          endforeach()
        endforeach()
      endforeach()
      continue()
    endif()
    foreach(_property IN LISTS ODBCPP_LIBRARY_LINK_PROPERTIES)
      get_target_property(_values ${_current} ${_property})
      if(NOT _values)
        continue()
      endif()
      foreach(_value IN LISTS _values)
        odbcpp_library_genex_problem("${_value}" _genex)
        if(_genex)
          set(${problem} "${_current}: ${_genex}" PARENT_SCOPE)
          return()
        endif()
        # Split on generator-expression syntax and whitespace, so paths stay whole
        # and "::" namespaces survive, but space-separated target lists inside genexes split.
        string(REPLACE "::" "@NS@" _text "${_value}")
        string(REGEX REPLACE "[$<>:, \t\r\n]+" ";" _text "${_text}")
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
    odbcpp_library_dependencies(${_library} "${_libraries}" _dependencies _problem)
    if(_problem)
      message(FATAL_ERROR "${_problem}")
    endif()
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
