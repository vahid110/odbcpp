# Build-only header staging that keeps unchanged copies stable across reconfigures.
# configure_file rewrites a copy only when its content changes, so objects that
# include an unchanged staged header stay up to date. Entries that are no longer
# selected are removed, so a dropped allowlist entry cannot linger in the stage.
#
# odbcpp_stage_headers(<stage> <logical> <source> [<logical> <source>...])
function(odbcpp_stage_headers stage)
  set(_pairs ${ARGN})
  list(LENGTH _pairs _count)
  math(EXPR _odd "${_count} % 2")
  if(_odd)
    message(FATAL_ERROR "Header staging needs <logical> <source> pairs: ${stage}")
  endif()
  # The stage is globbed below; glob metacharacters in it would hide strays.
  if(stage MATCHES "[][*?]")
    message(FATAL_ERROR "Header stage path must not contain glob characters: ${stage}")
  endif()
  set(_logicals "")
  set(_sources "")
  if(_count GREATER 0)
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE 0 ${_last} 2)
      math(EXPR _next "${_index} + 1")
      list(GET _pairs ${_index} _logical)
      list(GET _pairs ${_next} _source)
      # Plain '/'-separated names only: no drive, backslash, empty or dot segment.
      if(NOT _logical MATCHES "^[A-Za-z0-9_+-][A-Za-z0-9_.+-]*(/[A-Za-z0-9_+-][A-Za-z0-9_.+-]*)*$")
        message(FATAL_ERROR "Staged header must be a relative logical path: ${_logical}")
      endif()
      list(APPEND _logicals "${_logical}")
      list(APPEND _sources "${_source}")
    endforeach()
  endif()
  # Prune first, so a stale entry never blocks or survives the copies below.
  # A stage that is itself a link (or junction) is removed, never pruned through.
  if(IS_SYMLINK "${stage}")
    file(REMOVE "${stage}")
  elseif(IS_DIRECTORY "${stage}")
    file(GLOB_RECURSE _present LIST_DIRECTORIES false RELATIVE "${stage}" "${stage}/*")
    foreach(_entry IN LISTS _present)
      # A link is never reused: copies below must write regular files only.
      if(NOT _entry IN_LIST _logicals OR IS_SYMLINK "${stage}/${_entry}")
        file(REMOVE "${stage}/${_entry}")
      endif()
    endforeach()
    # Deepest first; a directory left without any selected header is removed.
    file(GLOB_RECURSE _directories LIST_DIRECTORIES true RELATIVE "${stage}" "${stage}/*")
    list(SORT _directories ORDER DESCENDING)
    foreach(_directory IN LISTS _directories)
      if(IS_DIRECTORY "${stage}/${_directory}" AND NOT IS_SYMLINK "${stage}/${_directory}")
        file(GLOB _children "${stage}/${_directory}/*")
        if(NOT _children)
          file(REMOVE_RECURSE "${stage}/${_directory}")
        endif()
      endif()
    endforeach()
  elseif(EXISTS "${stage}")
    file(REMOVE "${stage}")
  endif()
  foreach(_logical _source IN ZIP_LISTS _logicals _sources)
    configure_file("${_source}" "${stage}/${_logical}" COPYONLY)
  endforeach()
endfunction()

# Generated build inputs keep their timestamp when their content is unchanged.
# @ and ${} text is kept literally; #cmakedefine lines would still be rewritten
# by file(CONFIGURE), so they are refused.
function(odbcpp_write_if_changed path content)
  if(content MATCHES "#[ \t]*cmakedefine")
    message(FATAL_ERROR "Generated input must not contain #cmakedefine: ${path}")
  endif()
  string(REPLACE "@" "@ODBCPP_AT@" _escaped "${content}")
  set(ODBCPP_AT "@")
  file(CONFIGURE OUTPUT "${path}" CONTENT "${_escaped}" @ONLY)
endfunction()
