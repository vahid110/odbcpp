#pragma once
#include <windows.h>
#include "../setup_model.h"
#include <functional>
namespace rs::odbc::setup {
struct TestResult {
  bool succeeded = false;
  std::wstring details;
};
// Internal test seam: native UI tests inject a bounded, network-free operation.
using ConnectionTester = std::function<TestResult(const Fields&)>;
bool edit_dialog(HINSTANCE module, HWND parent, Fields& fields, bool fixed_name,
                 bool system_dsn = false, ConnectionTester tester = {});
}
