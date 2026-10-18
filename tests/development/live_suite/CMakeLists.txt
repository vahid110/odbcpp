cmake_minimum_required(VERSION 3.21)
project(ODBCPPExistingLiveSuite NONE)
enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/LiveSuite.cmake")
if(NOT ODBCPP_LIVE_OUTPUT)
  set(ODBCPP_LIVE_OUTPUT "${CMAKE_BINARY_DIR}/gtest")
endif()
odbcpp_add_live_suite(INDEX "${ODBCPP_LIVE_INDEX}" EXPECTED "${ODBCPP_LIVE_EXPECTED}"
  OUTPUT_DIRECTORY "${ODBCPP_LIVE_OUTPUT}")
