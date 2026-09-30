if(NOT DEFINED EXPECTED OR NOT EXISTS "${EXPECTED}" OR
   NOT DEFINED INVENTORY OR NOT EXISTS "${INVENTORY}")
  message(FATAL_ERROR "Driver export allowlist and test inventory are required")
endif()
file(STRINGS "${EXPECTED}" _expected)
list(FILTER _expected EXCLUDE REGEX "^[ \t]*(#.*)?$")
list(TRANSFORM _expected STRIP)
list(SORT _expected)

file(READ "${INVENTORY}" _inventory_source)
string(REGEX MATCHALL "\"SQL[A-Za-z0-9_]+\"" _inventory "${_inventory_source}")
list(TRANSFORM _inventory REPLACE "\"" "")
list(REMOVE_DUPLICATES _inventory)
list(SORT _inventory)
if(NOT _inventory STREQUAL _expected)
  message(FATAL_ERROR
    "SQLGetFunctions/wide export inventory differs from the canonical driver allowlist")
endif()
