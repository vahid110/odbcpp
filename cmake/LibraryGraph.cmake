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
# Generator expressions on those imported targets may only choose whether
# plain text is used; none may build or change text (#190). Conditions always
# evaluate to 0 or 1, whatever their arguments, and may nest inside each
# other. Each condition needs ":" ($<CONFIG> alone is the configuration name).
set(ODBCPP_LIBRARY_IMPORTED_CONDITIONS CONFIG PLATFORM_ID COMPILE_LANG_AND_ID
  LINK_LANG_AND_ID BOOL NOT AND OR)

# Sets <out> to the problem with one dependency entry <value> of an allowed
# imported target, or to "". Accepted: plain text; a bare condition (0 or 1);
# $<C:text>, $<0:text>,
# $<1:text> or $<LINK_ONLY:text> where C is a condition and text is plain;
# and $<LINK_ONLY:...> around one of those.
function(odbcpp_library_imported_genex_problem value out)
  string(FIND "${value}" "$<" _genex)
  if(_genex EQUAL -1)
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  # The placeholder cannot occur in a real value: "@" never appears in one
  # of the forms accepted below, and is refused in the input.
  if(value MATCHES "@")
    set(${out} "Imported target dependency may not contain '@' with a generator expression: ${value}" PARENT_SCOPE)
    return()
  endif()
  list(JOIN ODBCPP_LIBRARY_IMPORTED_CONDITIONS "|" _heads)
  set(_reduced "${value}")
  while(TRUE)
    # Innermost conditions first: their arguments hold no "$", "<" or ">".
    string(REGEX REPLACE "\\$<(${_heads}):[^$<>]*>" "@C@" _next "${_reduced}")
    if(_next STREQUAL _reduced)
      break()
    endif()
    set(_reduced "${_next}")
  endwhile()
  set(_text "[^$<>@]*")
  if(_reduced STREQUAL "@C@"
      OR _reduced MATCHES "^\\$<(@C@|0|1|LINK_ONLY):${_text}>$"
      OR _reduced MATCHES "^\\$<LINK_ONLY:\\$<(@C@|0|1):${_text}>>$")
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  set(${out} "Imported target dependency may only choose plain text with conditions (${ODBCPP_LIBRARY_IMPORTED_CONDITIONS}): ${value}" PARENT_SCOPE)
endfunction()

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

# Sets <links> and <files> to the properties of the imported target <target>
# that can add link items or files to a link line, including the ones CMake
# falls back to when INTERFACE_LINK_LIBRARIES is unset, each also with the
# suffix of every configuration this build could select (#202). A suffix none
# of these lists name is not judged.
function(odbcpp_library_imported_properties target links files)
  set(_configs NOCONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
  get_target_property(_imported_configs ${target} IMPORTED_CONFIGURATIONS)
  if(_imported_configs)
    list(APPEND _configs ${_imported_configs})
  endif()
  set(_mapped "")
  foreach(_config IN LISTS _configs)
    string(TOUPPER "${_config}" _upper)
    get_target_property(_map ${target} MAP_IMPORTED_CONFIG_${_upper})
    if(_map)
      list(APPEND _mapped ${_map})
    endif()
    if(DEFINED CMAKE_MAP_IMPORTED_CONFIG_${_upper})
      list(APPEND _mapped ${CMAKE_MAP_IMPORTED_CONFIG_${_upper}})
    endif()
  endforeach()
  set(_suffixes "")
  foreach(_config IN LISTS _configs _mapped)
    # CMake allows any characters in a configuration name (Rel-Asan, my.cfg),
    # and its property suffix is the name upper-cased, so judge every one.
    if(NOT _config STREQUAL "")
      string(TOUPPER "${_config}" _upper)
      list(APPEND _suffixes "_${_upper}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _suffixes)
  set(_links ${ODBCPP_LIBRARY_LINK_PROPERTIES})
  set(_files "")
  foreach(_suffix IN ITEMS "" ${_suffixes})
    list(APPEND _links IMPORTED_LINK_INTERFACE_LIBRARIES${_suffix}
      IMPORTED_LINK_DEPENDENT_LIBRARIES${_suffix} LINK_INTERFACE_LIBRARIES${_suffix})
    list(APPEND _files IMPORTED_LOCATION${_suffix} IMPORTED_IMPLIB${_suffix}
      IMPORTED_OBJECTS${_suffix} IMPORTED_LIBNAME${_suffix})
  endforeach()
  set(${links} "${_links}" PARENT_SCOPE)
  set(${files} "${_files}" PARENT_SCOPE)
endfunction()

# Sets <out> to TRUE if the link item <token>, which is not a target where
# the check runs (the top directory), may still be one where it is used: an
# imported target created without GLOBAL in a subdirectory is visible only
# there, so its properties cannot be judged. Namespaced names ("x::y") are
# always targets in CMake; other names count if any directory imports them,
# even when the top directory means a system library by that name (fails
# closed). Without IMPORTED_TARGETS (CMake 3.20) only namespaced names count.
# "::@(...)" markers, which CMake adds around items linked from another
# directory, are not targets.
function(odbcpp_library_hidden_target token out)
  get_property(_imported GLOBAL PROPERTY ODBCPP_LIBRARY_ALL_IMPORTED)
  if(token STREQUAL "")
    set(${out} FALSE PARENT_SCOPE)
  elseif(token IN_LIST _imported OR (token MATCHES "::" AND NOT token MATCHES "^::@"))
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

# Sets <out> to the imported targets of <dir> and all its subdirectories.
function(odbcpp_library_imported_targets dir out)
  set(_all "")
  if(NOT CMAKE_VERSION VERSION_LESS 3.21)
    get_directory_property(_here DIRECTORY "${dir}" IMPORTED_TARGETS)
    list(APPEND _all ${_here})
  endif()
  get_directory_property(_subdirs DIRECTORY "${dir}" SUBDIRECTORIES)
  foreach(_sub IN LISTS _subdirs)
    odbcpp_library_imported_targets("${_sub}" _more)
    list(APPEND _all ${_more})
  endforeach()
  set(${out} "${_all}" PARENT_SCOPE)
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
      # Names in an imported target's links resolve in the directory that
      # created it; only the top directory is the one the check sees.
      if(NOT CMAKE_VERSION VERSION_LESS 3.21)
        get_directory_property(_top_imported DIRECTORY "${CMAKE_SOURCE_DIR}" IMPORTED_TARGETS)
        if(NOT _current IN_LIST _top_imported)
          set(${problem} "Imported target ${_current} must be created in the top directory, where the library graph check runs" PARENT_SCOPE)
          return()
        endif()
      endif()
      odbcpp_library_imported_properties(${_current} _link_properties _file_properties)
      foreach(_property IN LISTS _file_properties)
        get_target_property(_values ${_current} ${_property})
        if(NOT _values)
          continue()
        endif()
        foreach(_value IN LISTS _values)
          # The file itself may not be a product library. Only its name is
          # judged: in-tree crypto builds live under the build directory, whose
          # path may contain "odbcpp".
          get_filename_component(_name "${_value}" NAME)
          string(TOLOWER "${_name}" _lower)
          if(_value MATCHES "\\$<" OR _lower MATCHES "odbcpp")
            set(${problem} "Imported target ${_current} may not use this file (${_property}): ${_value}" PARENT_SCOPE)
            return()
          endif()
        endforeach()
      endforeach()
      foreach(_property IN LISTS _link_properties)
        get_target_property(_values ${_current} ${_property})
        if(NOT _values)
          continue()
        endif()
        foreach(_value IN LISTS _values)
          # Any reference to a product target (direct, namespaced, or assembled)
          # inside an allowed imported target's properties is forbidden.
          # The whole value is judged, so a link item that is a path under a
          # directory named odbcpp is refused too (fails closed); imported
          # files below judge only the file name.
          string(TOLOWER "${_value}" _lower)
          if(_lower MATCHES "odbcpp")
            set(${problem} "Imported target ${_current} may not depend on product target: ${_value}" PARENT_SCOPE)
            return()
          endif()
          # Dynamic target generator expressions (e.g. TARGET_PROPERTY, GENEX_EVAL, TARGET_NAME)
          # can evaluate to arbitrary targets at generate time and are forbidden on imported targets.
          if(_value MATCHES "TARGET_|GENEX_EVAL")
            set(${problem} "Imported target ${_current} may not use dynamic generator expressions: ${_value}" PARENT_SCOPE)
            return()
          endif()
          # Assembled names ($<1:odb>$<1:cpp_tls>, odb$<1:cpp_tls>,
          # $<1:odb$<1:cpp_tls>>) and text-changing expressions ($<LOWER_CASE:...>)
          # evade tokenization; only conditions choosing plain text are allowed.
          odbcpp_library_imported_genex_problem("${_value}" _genex)
          if(_genex)
            set(${problem} "Imported target ${_current} may not use this generator expression. ${_genex}" PARENT_SCOPE)
            return()
          endif()
          # Split on generator-expression syntax and whitespace, so paths stay whole
          # but space-separated lists are tokenized; "::" namespaces survive.
          string(REPLACE "::" "@NS@" _text "${_value}")
          string(REGEX REPLACE "[$<>:, \t\r\n]+" ";" _text "${_text}")
          string(REPLACE "@NS@" "::" _tokens "${_text}")
          foreach(_token IN LISTS _tokens)
            if(NOT TARGET "${_token}")
              odbcpp_library_hidden_target("${_token}" _hidden)
              if(_hidden)
                set(${problem} "Imported target ${_current} links ${_token}, a target the check cannot see (create imported targets GLOBAL or in the top directory)" PARENT_SCOPE)
                return()
              endif()
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
    # Link names resolve in the directory that declared them. The check sees
    # only the top directory, so every target it walks must be defined there
    # and linked from there; otherwise an imported target or alias local to a
    # subdirectory could hide behind a plain name.
    get_target_property(_source_dir ${_current} SOURCE_DIR)
    if(NOT _source_dir STREQUAL CMAKE_SOURCE_DIR)
      set(${problem} "${_current} must be defined in the top directory, where the library graph check runs (it is defined in ${_source_dir})" PARENT_SCOPE)
      return()
    endif()
    foreach(_property IN LISTS ODBCPP_LIBRARY_LINK_PROPERTIES)
      get_target_property(_values ${_current} ${_property})
      if(NOT _values)
        continue()
      endif()
      foreach(_value IN LISTS _values)
        if(_value MATCHES "^::@")
          set(${problem} "${_current} has links declared in another directory; declare them in the top directory, where the library graph check runs" PARENT_SCOPE)
          return()
        endif()
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
            odbcpp_library_hidden_target("${_token}" _hidden)
            if(_hidden)
              set(${problem} "${_current} links ${_token}, a target the check cannot see (create imported targets GLOBAL or in the top directory)" PARENT_SCOPE)
              return()
            endif()
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
  odbcpp_library_imported_targets("${CMAKE_SOURCE_DIR}" _all_imported)
  set_property(GLOBAL PROPERTY ODBCPP_LIBRARY_ALL_IMPORTED "${_all_imported}")
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
