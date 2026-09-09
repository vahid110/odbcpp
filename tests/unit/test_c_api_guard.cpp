#include <gtest/gtest.h>

#include "odbc/c_api_guard.h"
#include "odbc/odbc_api.h"
#include "tests/test_handle_helpers.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void expect_serialized_callbacks(const std::vector<SQLHANDLE>& handles) {
  constexpr std::size_t thread_count = 6;
  constexpr std::size_t iterations = 400;
  std::atomic<std::size_t> ready{0};
  std::atomic<bool> start{false};
  std::atomic<int> active{0};
  std::atomic<int> maximum_active{0};
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;

  for (std::size_t thread_index = 0; thread_index < thread_count;
       ++thread_index) {
    threads.emplace_back([&, thread_index] {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      const auto handle = handles[thread_index % handles.size()];
      for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto result = rs::odbc::detail::invoke_c_api(
            handle, [&] {
              const auto concurrent =
                  active.fetch_add(1, std::memory_order_acq_rel) + 1;
              auto observed = maximum_active.load(std::memory_order_relaxed);
              while (concurrent > observed &&
                     !maximum_active.compare_exchange_weak(
                         observed, concurrent, std::memory_order_relaxed)) {
              }
              std::this_thread::yield();
              active.fetch_sub(1, std::memory_order_acq_rel);
              return SQL_SUCCESS;
            });
        if (result != SQL_SUCCESS) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  while (ready.load(std::memory_order_acquire) != thread_count) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();

  EXPECT_EQ(0, failures.load());
  EXPECT_EQ(1, maximum_active.load());
}

TEST(CApiGuardTest, ConvertsUnexpectedExceptionToDiagnosticError) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));

  EXPECT_EQ(SQL_ERROR, rs::odbc::detail::invoke_c_api(
                           environment, []() -> SQLRETURN {
              throw std::runtime_error("injected failure");
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

TEST(CApiGuardTest, SerializesConcurrentOperationsOnOneHandle) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));

  expect_serialized_callbacks({environment});

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(CApiGuardTest, SerializesSiblingOperationsThroughTheirAncestors) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  const auto first = odbcpp::test::make_statement(connection);
  const auto second = odbcpp::test::make_statement(connection);
  ASSERT_NE(nullptr, first);
  ASSERT_NE(nullptr, second);

  expect_serialized_callbacks({first, second});

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, first));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, second));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(CApiGuardTest, KeepsIndependentConnectionsConcurrent) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC first = SQL_NULL_HDBC;
  SQLHDBC second = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &first));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &second));

  std::atomic<bool> start{false};
  std::atomic<int> callbacks_entered{0};
  std::atomic<int> rendezvous_timeouts{0};
  std::atomic<int> failures{0};
  auto operation = [&](SQLHDBC connection) {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto result = rs::odbc::detail::invoke_c_api(connection, [&] {
      callbacks_entered.fetch_add(1, std::memory_order_acq_rel);
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (callbacks_entered.load(std::memory_order_acquire) != 2 &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
      }
      if (callbacks_entered.load(std::memory_order_acquire) != 2) {
        rendezvous_timeouts.fetch_add(1, std::memory_order_relaxed);
      }
      return SQL_SUCCESS;
    });
    if (result != SQL_SUCCESS) failures.fetch_add(1, std::memory_order_relaxed);
  };

  std::thread first_thread(operation, first);
  std::thread second_thread(operation, second);
  start.store(true, std::memory_order_release);
  first_thread.join();
  second_thread.join();

  EXPECT_EQ(2, callbacks_entered.load());
  EXPECT_EQ(0, rendezvous_timeouts.load());
  EXPECT_EQ(0, failures.load());
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, first));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, second));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

}  // namespace
