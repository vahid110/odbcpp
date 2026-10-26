#pragma once

// Application-side ODBC declarations for a suite compiled against a Driver
// Manager (ODBCPP_TEST_VIA_DRIVER_MANAGER). Only the manager's own headers are
// used: such a target links the manager and never odbcpp_core, so any driver
// internal left in the suite fails to link instead of bypassing the manager.
#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
#error "tests/test_dm_odbc.h is only for Driver Manager suite builds"
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

// ODBC 3.8 identifier absent from older iODBC headers.
#ifndef SQL_API_SQLCANCELHANDLE
#define SQL_API_SQLCANCELHANDLE 1022
#endif

#ifdef ODBCPP_EXPECT_DM_SQLWCHAR_SIZE
static_assert(sizeof(SQLWCHAR) == ODBCPP_EXPECT_DM_SQLWCHAR_SIZE,
              "The Driver Manager headers have an unexpected SQLWCHAR ABI");
#endif
