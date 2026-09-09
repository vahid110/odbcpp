#include <gtest/gtest.h>

#include "odbc/c_api_guard.h"
#include "odbc/odbc_api.h"

#include <stdexcept>

namespace {

TEST(CApiGuardTest, ConvertsUnexpectedExceptionToDiagnosticError) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));

  EXPECT_EQ(SQL_ERROR, rs::odbc::detail::invoke_c_api(environment, [] {
              throw std::runtime_error("injected failure");
              return SQL_SUCCESS;
            }));

  SQLCHAR state[6]{};
  SQLCHAR message[128]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(SQL_HANDLE_ENV, environment, 1, state, nullptr,
                          message, sizeof(message), nullptr));
  EXPECT_STREQ("HY000", reinterpret_cast<const char*>(state));
  EXPECT_STREQ("Unexpected internal driver exception",
               reinterpret_cast<const char*>(message));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(CApiGuardTest, DiagnosticPathDoesNotThrowWithoutAUsableHandle) {
  EXPECT_EQ(SQL_ERROR,
            rs::odbc::detail::invoke_c_api(SQL_NULL_HANDLE, []() -> SQLRETURN {
              throw 42;
            }));
}

}  // namespace
