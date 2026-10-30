# Script-mode checks that the library graph rules accept allowed links and
# reject every kind of violation (ADR 0002).
include("${CMAKE_CURRENT_LIST_DIR}/LibraryGraph.cmake")

function(expect actual expected label)
  if(NOT "${actual}" STREQUAL "${expected}")
    message(FATAL_ERROR "${label}: expected '${expected}', got '${actual}'")
  endif()
endfunction()

set(_graph "odbcpp_base=" "odbcpp_tls=odbcpp_base" "odbcpp_odbc=odbcpp_base,odbcpp_tls")

odbcpp_library_graph_problem("${ODBCPP_LIBRARY_GRAPH}" _problem)
expect("${_problem}" "" "checked-in graph")
odbcpp_library_graph_problem("${_graph}" _problem)
expect("${_problem}" "" "valid graph")
odbcpp_library_graph_problem("odbcpp_base=;odbcpp_base=" _problem)
expect("${_problem}" "Duplicate library graph entry: odbcpp_base" "duplicate entry")
odbcpp_library_graph_problem("odbcpp_base:odbcpp_tls" _problem)
expect("${_problem}" "Malformed library graph entry: odbcpp_base:odbcpp_tls" "malformed entry")

odbcpp_library_link_problem("${_graph}" odbcpp_odbc "odbcpp_base;$<LINK_ONLY:odbcpp_tls>;Threads::Threads" _problem)
expect("${_problem}" "" "allowed links and non-product links")
odbcpp_library_link_problem("${_graph}" odbcpp_base "odbcpp_tls" _problem)
expect("${_problem}" "Library odbcpp_base may not depend on odbcpp_tls" "disallowed edge")
odbcpp_library_link_problem("${_graph}" odbcpp_tls "$<LINK_ONLY:odbcpp_odbc>" _problem)
expect("${_problem}" "Library odbcpp_tls may not depend on odbcpp_odbc" "wrapped disallowed edge")
odbcpp_library_link_problem("${_graph}" odbcpp_base "odbcpp::core" _problem)
expect("${_problem}" "Library odbcpp_base may not depend on odbcpp::core" "namespaced target")
odbcpp_library_link_problem("${_graph}" odbcpp_unknown "" _problem)
expect("${_problem}" "Library odbcpp_unknown is not in the library graph" "unknown library")
message(STATUS "library graph checks passed")
