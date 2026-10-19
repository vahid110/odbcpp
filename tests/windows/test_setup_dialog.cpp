#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <gtest/gtest.h>

#include "odbc/setup/setup_dialog.h"
#include "odbc/setup/resource.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace rs::odbc::setup;
using Clock = std::chrono::steady_clock;

struct Observations {
  std::vector<std::string> failures;
  void check(bool condition, const char* description) {
    if (!condition) failures.emplace_back(description);
  }
};

bool send(HWND window, UINT message, WPARAM wp = 0, LPARAM lp = 0,
          DWORD_PTR* result = nullptr) {
  DWORD_PTR ignored = 0;
  return SendMessageTimeoutW(window, message, wp, lp, SMTO_ABORTIFHUNG,
                            1000, result ? result : &ignored) != 0;
}
bool click(HWND window, int id) {
  return send(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
              reinterpret_cast<LPARAM>(GetDlgItem(window, id)));
}
bool set_text(HWND window, int id, const wchar_t* value) {
  return send(GetDlgItem(window, id), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value));
}
std::wstring text(HWND window, int id) {
  wchar_t buffer[4096]{};
  if (!send(GetDlgItem(window, id), WM_GETTEXT, 4096, reinterpret_cast<LPARAM>(buffer))) return {};
  return buffer;
}
bool wait_for(const std::function<bool()>& condition, int milliseconds = 3000) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
  do {
    if (condition()) return true;
    Sleep(5);
  } while (Clock::now() < deadline);
  return condition();
}
HWND find_dialog(DWORD thread_id) {
  struct Search { const std::wstring title = driver_name() + L" Setup"; HWND found = nullptr; } search;
  EnumThreadWindows(thread_id, [](HWND window, LPARAM input) -> BOOL {
    auto& state = *reinterpret_cast<Search*>(input);
    wchar_t title[256]{};
    GetWindowTextW(window, title, 256);
    if (state.title == title) { state.found = window; return FALSE; }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.found;
}
bool select_page(HWND window, int page) {
  if (!send(GetDlgItem(window, IDC_TABS), TCM_SETCURSEL, page)) return false;
  NMHDR notification{GetDlgItem(window, IDC_TABS), IDC_TABS, TCN_SELCHANGE};
  return send(window, WM_NOTIFY, IDC_TABS, reinterpret_cast<LPARAM>(&notification));
}
bool checked(HWND window, int id) {
  DWORD_PTR state = 0;
  return send(GetDlgItem(window, id), BM_GETCHECK, 0, 0, &state) && state == BST_CHECKED;
}
bool check_box(HWND window, int id, bool value) {
  return send(GetDlgItem(window, id), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED) && click(window, id);
}

bool capture(HWND window, const wchar_t* filename) {
  wchar_t directory[32768]{};
  const auto length = GetEnvironmentVariableW(L"ODBCPP_GUI_CAPTURE_DIR", directory, 32768);
  if (length == 0) return true;
  if (length >= 32768) return false;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  if (!RedrawWindow(window, nullptr, nullptr,
                    RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN)) return false;
  RECT rect{};
  if (!GetWindowRect(window, &rect)) return false;
  const int width = rect.right - rect.left, height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  const auto dc = GetDC(window);
  const auto memory = dc ? CreateCompatibleDC(dc) : nullptr;
  void* pixels = nullptr;
  const auto bitmap = memory ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
  bool ok = bitmap && pixels;
  HGDIOBJ previous = nullptr;
  if (ok) {
    previous = SelectObject(memory, bitmap);
    ok = PrintWindow(window, memory, 2) != FALSE;
    if (ok) {
      const auto path = std::filesystem::path(directory) / filename;
      const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      ok = file != INVALID_HANDLE_VALUE;
      if (ok) {
        const auto bytes = static_cast<DWORD>(width * height * 4);
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + bytes;
        DWORD written = 0;
        ok = WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header);
        ok = ok && WriteFile(file, &info.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr) && written == sizeof(BITMAPINFOHEADER);
        ok = ok && WriteFile(file, pixels, bytes, &written, nullptr) && written == bytes;
        CloseHandle(file);
      }
    }
  }
  if (previous) SelectObject(memory, previous);
  if (bitmap) DeleteObject(bitmap);
  if (memory) DeleteDC(memory);
  if (dc) ReleaseDC(window, dc);
  return ok;
}

struct DialogRun { bool accepted = false; std::vector<std::string> failures; };
DialogRun run_dialog(Fields& fields, bool system_dsn, ConnectionTester tester,
                     const std::function<void(HWND, Observations&)>& actions) {
  Observations observations;
  const auto ui_thread = GetCurrentThreadId();
  // Ensure the UI thread has a message queue before automation can post to it.
  MSG queued{};
  PeekMessageW(&queued, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  std::thread automation([&] {
    HWND window = nullptr;
    if (!wait_for([&] {
      window = find_dialog(ui_thread);
      return window != nullptr && IsWindowVisible(window);
    })) {
      observations.check(false, "Native setup dialog did not appear within three seconds");
      PostThreadMessageW(ui_thread, WM_QUIT, 0, 0);
      return;
    }
    try { actions(window, observations); }
    catch (...) { observations.check(false, "Native dialog automation threw an exception"); }
    // Every automation path requests closure. The injected tester has a finite
    // deadline, allowing the production deferred-close path to join its worker.
    if (IsWindow(window)) {
      PostMessageW(window, WM_CLOSE, 0, 0);
      observations.check(wait_for([&] { return !IsWindow(window); }), "Dialog did not finish closing");
    }
  });
  DialogRun result;
  bool dialog_failed = false;
  try { result.accepted = edit_dialog(GetModuleHandleW(nullptr), nullptr, fields, true, system_dsn, std::move(tester)); }
  catch (...) { dialog_failed = true; }
  automation.join();
  observations.check(!dialog_failed, "Native edit_dialog failed");
  while (PeekMessageW(&queued, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
  result.failures = std::move(observations.failures);
  return result;
}
Fields initial_fields() {
  Fields fields;
  fields.dsn = L"ODBCPP_Native_UI_Test";
  fields.server = L"offline.invalid";
  fields.database = L"test";
  fields.user = L"test-user";
  return fields;
}
struct BoundedFake {
  std::atomic<bool> started{false}, release{false}, finished{false};
  std::mutex mutex;
  Attributes observed;
  std::wstring observed_password;
  TestResult run(const Fields& fields, bool succeeded) {
    {
      std::lock_guard lock(mutex);
      observed = fields.advanced;
      observed_password = fields.password;
    }
    started.store(true);
    const auto began = Clock::now();
    wait_for([&] { return release.load(); }, 2000);
    const auto minimum = began + std::chrono::milliseconds(150);
    if (Clock::now() < minimum) std::this_thread::sleep_until(minimum);
    finished.store(true);
    return {succeeded, succeeded ? L"Offline injected success details." : L"Offline injected failure details. SQLSTATE 08001."};
  }
  ~BoundedFake() { secure_clear(observed_password); }
};

TEST(SetupDialog, NativePagesDraftTestingAndSaveRemainResponsiveWithoutNetwork) {
  auto fields = initial_fields();
  fields.advanced[L"APPLICATIONNAME"] = L"original-app";
  fields.advanced[L"LOGINTIMEOUT"] = L"9";
  fields.advanced[L"FUTURESETTING"] = L"retained";
  BoundedFake fake;
  const auto result = run_dialog(fields, true, [&](const Fields& draft) { return fake.run(draft, true); },
      [&](HWND window, Observations& checks) {
    DWORD_PTR tab_count = 0;
    checks.check(send(GetDlgItem(window, IDC_TABS), TCM_GETITEMCOUNT, 0, 0, &tab_count) && tab_count == 4, "Four native pages must be present");
    checks.check(text(window, IDC_SCOPE).find(L"System data source") != std::wstring::npos, "System scope must be visible");
    checks.check(text(window, IDC_HEADING) == from_utf8(rs::core::database::configured_backend_provider().identity().display_name), "Heading must use configured provider identity");
    DWORD_PTR heading_font = 0;
    LOGFONTW heading_description{};
    checks.check(send(GetDlgItem(window, IDC_HEADING), WM_GETFONT, 0, 0, &heading_font) &&
                   GetObjectW(reinterpret_cast<HFONT>(heading_font), sizeof(heading_description), &heading_description) &&
                   std::wstring(heading_description.lfFaceName) == L"Segoe UI",
                 "Heading must use the explicit Segoe UI presentation font");
    checks.check(!IsWindowEnabled(GetDlgItem(window, IDC_DSN)), "Existing DSN name must remain fixed");
    checks.check((GetWindowLongPtrW(GetDlgItem(window, IDC_PASSWORD), GWL_STYLE) & ES_PASSWORD) != 0, "Test password must be masked");
    checks.check(IsWindowVisible(GetDlgItem(window, IDC_SERVER)) && !IsWindowVisible(GetDlgItem(window, IDC_PASSWORD)), "Connection page must show connection fields only");
    checks.check(capture(window, L"setup-connection.bmp"), "Capture Connection page");
    checks.check(set_text(window, IDC_PORT, L"0") && click(window, IDOK), "Submit invalid port");
    checks.check(IsWindow(window) && text(window, IDC_STATUS).find(L"Port") != std::wstring::npos, "Invalid port must retain dialog and explain validation");
    checks.check(set_text(window, IDC_PORT, L"5432") && set_text(window, IDC_APP, L"draft-app"), "Set valid connection draft");
    checks.check(select_page(window, 1), "Select Security page");
    checks.check(IsWindowVisible(GetDlgItem(window, IDC_PASSWORD)) && !IsWindowVisible(GetDlgItem(window, IDC_SERVER)), "Security page visibility");
    checks.check(set_text(window, IDC_PASSWORD, L"offline-test-secret"), "Set transient test password");
    checks.check(capture(window, L"setup-security.bmp"), "Capture Security page");
    checks.check(select_page(window, 2), "Select Advanced page");
    checks.check(IsWindowVisible(GetDlgItem(window, IDC_PROPERTIES)) && !IsWindowVisible(GetDlgItem(window, IDC_PASSWORD)), "Advanced page visibility");
    checks.check(checked(window, IDC_PROPERTY_OVERRIDE), "Stored login override must be selected");
    checks.check(set_text(window, IDC_PROPERTY_VALUE, L"17"), "Edit login timeout override");
    checks.check(capture(window, L"setup-advanced.bmp"), "Capture Advanced page");
    checks.check(select_page(window, 3), "Select Diagnostics page");
    checks.check(IsWindowVisible(GetDlgItem(window, IDC_DETAILS)) && !IsWindowVisible(GetDlgItem(window, IDC_PROPERTIES)), "Diagnostics page visibility");
    checks.check(click(window, IDC_TEST), "Start injected connection test");
    checks.check(wait_for([&] { return fake.started.load(); }), "Worker must receive the draft");
    checks.check(!IsWindowEnabled(GetDlgItem(window, IDOK)), "Save must be disabled while testing");
    checks.check(send(window, WM_NULL), "UI must process messages while worker is pending");
    checks.check(!fake.finished.load(), "Responsiveness must be observed before worker completion");
    fake.release.store(true);
    checks.check(wait_for([&] { return IsWindowEnabled(GetDlgItem(window, IDOK)) != FALSE; }), "Test completion must re-enable Save");
    const auto details = text(window, IDC_DETAILS);
    checks.check(details.find(L"Offline injected success details.") != std::wstring::npos && details.find(L"Elapsed:") != std::wstring::npos, "Diagnostics must show result and elapsed time");
    checks.check(details.find(L"offline-test-secret") == std::wstring::npos, "Details must exclude test password");
    checks.check(capture(window, L"setup-diagnostics.bmp"), "Capture Diagnostics page");
    checks.check(click(window, IDOK), "Save tested draft");
  });
  EXPECT_TRUE(result.accepted);
  for (const auto& failure : result.failures) {
    ADD_FAILURE() << failure;
  }
  EXPECT_EQ(L"draft-app", fields.advanced.at(L"APPLICATIONNAME"));
  EXPECT_EQ(L"17", fields.advanced.at(L"LOGINTIMEOUT"));
  EXPECT_EQ(L"retained", fields.advanced.at(L"FUTURESETTING"));
  EXPECT_TRUE(fields.password.empty());
  std::lock_guard lock(fake.mutex);
  EXPECT_EQ(fields.advanced, fake.observed);
  EXPECT_EQ(L"offline-test-secret", fake.observed_password);
}

TEST(SetupDialog, InheritanceResetRemovesOverridesOnSave) {
  auto fields = initial_fields();
  fields.advanced[L"APPLICATIONNAME"] = L"original-app";
  fields.advanced[L"LOGINTIMEOUT"] = L"9";
  const auto result = run_dialog(fields, false, [](const Fields&) { return TestResult{false, L"Unused offline tester"}; },
      [](HWND window, Observations& checks) {
    checks.check(text(window, IDC_SCOPE).find(L"User data source") != std::wstring::npos, "User scope must be visible");
    checks.check(check_box(window, IDC_APP_DEFAULT, true), "Restore application inheritance");
    checks.check(!IsWindowEnabled(GetDlgItem(window, IDC_APP)), "Inherited application field must be disabled");
    checks.check(select_page(window, 2) && check_box(window, IDC_PROPERTY_OVERRIDE, false), "Restore login timeout inheritance");
    checks.check(!IsWindowEnabled(GetDlgItem(window, IDC_PROPERTY_VALUE)), "Inherited property value must be disabled");
    checks.check(click(window, IDOK), "Save inheritance reset");
  });
  EXPECT_TRUE(result.accepted);
  for (const auto& failure : result.failures) {
    ADD_FAILURE() << failure;
  }
  EXPECT_EQ(0u, fields.advanced.count(L"APPLICATIONNAME"));
  EXPECT_EQ(0u, fields.advanced.count(L"LOGINTIMEOUT"));
  EXPECT_EQ(1u, fields.removed.count(L"APPLICATIONNAME"));
  EXPECT_EQ(1u, fields.removed.count(L"LOGINTIMEOUT"));
}

TEST(SetupDialog, UntouchedEmptyTrustOverridesArePreserved) {
  auto fields = initial_fields();
  fields.advanced[L"SSLCAFILE"] = L"";
  fields.advanced[L"SSLCADIR"] = L"";
  const auto result = run_dialog(fields, false, {}, [](HWND window, Observations& checks) {
    checks.check(click(window, IDOK), "Save without editing trust settings");
  });
  EXPECT_TRUE(result.accepted);
  for (const auto& failure : result.failures) {
    ADD_FAILURE() << failure;
  }
  EXPECT_EQ(1u, fields.advanced.count(L"SSLCAFILE"));
  EXPECT_EQ(1u, fields.advanced.count(L"SSLCADIR"));
  EXPECT_EQ(L"", fields.advanced.at(L"SSLCAFILE"));
  EXPECT_EQ(L"", fields.advanced.at(L"SSLCADIR"));
}

TEST(SetupDialog, SelectingTrustDirectoryClearsInheritedFileWithExplicitEmptyOverride) {
  auto fields = initial_fields();
  const auto result = run_dialog(fields, false, {}, [](HWND window, Observations& checks) {
    checks.check(select_page(window, 1), "Select Security page");
    checks.check(send(GetDlgItem(window, IDC_TRUST), CB_SETCURSEL, 2), "Choose directory trust");
    checks.check(send(window, WM_COMMAND, MAKEWPARAM(IDC_TRUST, CBN_SELCHANGE),
                       reinterpret_cast<LPARAM>(GetDlgItem(window, IDC_TRUST))), "Notify trust selection");
    checks.check(set_text(window, IDC_CA_PATH, L"C:\\test-trust"), "Set trust directory draft");
    checks.check(click(window, IDOK), "Save trust directory draft");
  });
  EXPECT_TRUE(result.accepted);
  for (const auto& failure : result.failures) {
    ADD_FAILURE() << failure;
  }
  EXPECT_EQ(L"", fields.advanced.at(L"SSLCAFILE"));
  EXPECT_EQ(L"C:\\test-trust", fields.advanced.at(L"SSLCADIR"));
}

TEST(SetupDialog, CloseDuringFailedTestDefersUntilWorkerFinishesAndDoesNotSave) {
  auto fields = initial_fields();
  fields.advanced[L"APPLICATIONNAME"] = L"original-app";
  BoundedFake fake;
  const auto result = run_dialog(fields, false, [&](const Fields& draft) { return fake.run(draft, false); },
      [&](HWND window, Observations& checks) {
    checks.check(set_text(window, IDC_SERVER, L"edited.invalid") && set_text(window, IDC_APP, L"discarded-app"), "Edit draft without saving");
    checks.check(click(window, IDC_TEST) && wait_for([&] { return fake.started.load(); }), "Start bounded failing tester");
    checks.check(send(window, WM_CLOSE), "Request close while test is running");
    checks.check(IsWindow(window) && !IsWindowEnabled(GetDlgItem(window, IDCANCEL)), "Close must be deferred with Cancel disabled");
    checks.check(text(window, IDC_STATUS).find(L"Closing when") != std::wstring::npos, "Deferred close must be explained");
    checks.check(send(window, WM_NULL) && !fake.finished.load(), "Deferred close must preserve responsive live UI until worker completes");
    fake.release.store(true);
    checks.check(wait_for([&] { return !IsWindow(window); }), "Dialog must close after the worker finishes");
  });
  EXPECT_FALSE(result.accepted);
  for (const auto& failure : result.failures) {
    ADD_FAILURE() << failure;
  }
  EXPECT_TRUE(fake.finished.load());
  EXPECT_EQ(L"offline.invalid", fields.server);
  EXPECT_EQ(L"original-app", fields.advanced.at(L"APPLICATIONNAME"));
}

TEST(SetupDialog, FailedTestPublishesDetailsAndLeavesDraftUnsavedOnCancel) {
  auto fields = initial_fields();
  BoundedFake fake;
  const auto result = run_dialog(fields, false, [&](const Fields& draft) { return fake.run(draft, false); },
      [&](HWND window, Observations& checks) {
    checks.check(set_text(window, IDC_SERVER, L"discarded.invalid"), "Edit connection draft");
    checks.check(click(window, IDC_TEST) && wait_for([&] { return fake.started.load(); }), "Start injected failed connection test");
    fake.release.store(true);
    checks.check(wait_for([&] { return IsWindowEnabled(GetDlgItem(window, IDOK)) != FALSE; }), "Failed test must re-enable Save");
    checks.check(text(window, IDC_STATUS).find(L"Connection failed") != std::wstring::npos, "Failed test must report failure");
    checks.check(select_page(window, 3), "Open failed test diagnostics");
    const auto details = text(window, IDC_DETAILS);
    checks.check(details.find(L"Offline injected failure details. SQLSTATE 08001.") != std::wstring::npos &&
                 details.find(L"Elapsed:") != std::wstring::npos, "Failed test diagnostics must contain structured result and elapsed time");
    checks.check(click(window, IDCANCEL), "Cancel unsaved failed draft");
  });
  EXPECT_FALSE(result.accepted);
  for (const auto& failure : result.failures) {
    ADD_FAILURE() << failure;
  }
  EXPECT_TRUE(fake.finished.load());
  EXPECT_EQ(L"offline.invalid", fields.server);
}
}  // namespace
