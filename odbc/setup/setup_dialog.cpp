#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <sql.h>
#include <sqlext.h>
#include <odbcinst.h>
#include "setup_dialog.h"
#include "resource.h"
#include <chrono>
#include <thread>
#include <utility>

namespace rs::odbc::setup {
namespace {
constexpr UINT test_finished = WM_APP + 17;
struct Dialog {
  Fields* fields;
  Fields draft;
  bool fixed_name, system_dsn, busy = false, close_when_finished = false, updating = false;
  bool trust_touched = false;
  int selected_property = -1;
  std::vector<Property> properties;
  ConnectionTester tester;
  std::thread worker;
  TestResult result;
  HFONT heading_font = nullptr;
  ~Dialog() {
    if (worker.joinable()) worker.join();
    if (heading_font) DeleteObject(heading_font);
  }
};

std::wstring edit(HWND window, int id) {
  const auto control = GetDlgItem(window, id);
  const int size = GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(size) + 1, L'\0');
  GetWindowTextW(control, value.data(), size + 1);
  value.resize(size);
  return value;
}
bool checked(HWND w, int id) { return IsDlgButtonChecked(w, id) == BST_CHECKED; }
void text(HWND w, int id, const std::wstring& value) { SetDlgItemTextW(w, id, value.c_str()); }
void inherit(Fields& fields, const std::wstring& key) {
  fields.advanced.erase(key);
  fields.removed.insert(key);
}

void store_property(HWND w, Dialog& state) {
  if (state.selected_property < 0) return;
  const auto& property = state.properties.at(static_cast<std::size_t>(state.selected_property));
  if (checked(w, IDC_PROPERTY_OVERRIDE)) {
    state.draft.advanced[property.key] = edit(w, IDC_PROPERTY_VALUE);
    state.draft.removed.erase(property.key);
  } else inherit(state.draft, property.key);
  const auto found = state.draft.advanced.find(property.key);
  auto value = found == state.draft.advanced.end() ? L"Driver default" :
      found->second.empty() ? L"(empty)" : found->second;
  LVITEMW item{};
  item.iSubItem = 1;
  item.pszText = value.data();
  SendDlgItemMessageW(w, IDC_PROPERTIES, LVM_SETITEMTEXTW,
                     state.selected_property, reinterpret_cast<LPARAM>(&item));
}
void load_property(HWND w, Dialog& state, int index) {
  state.selected_property = index;
  if (index < 0) return;
  const auto& property = state.properties.at(static_cast<std::size_t>(index));
  const auto found = state.draft.advanced.find(property.key);
  CheckDlgButton(w, IDC_PROPERTY_OVERRIDE, found == state.draft.advanced.end() ? BST_UNCHECKED : BST_CHECKED);
  SendDlgItemMessageW(w, IDC_PROPERTY_VALUE, CB_RESETCONTENT, 0, 0);
  if (property.kind == PropertyKind::Boolean) {
    for (const auto* value : {L"0", L"1"})
      SendDlgItemMessageW(w, IDC_PROPERTY_VALUE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
  }
  for (const auto& value : property.choices)
    SendDlgItemMessageW(w, IDC_PROPERTY_VALUE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
  text(w, IDC_PROPERTY_VALUE, found == state.draft.advanced.end() ? L"" : found->second);
  text(w, IDC_PROPERTY_DESCRIPTION, property.description);
  EnableWindow(GetDlgItem(w, IDC_PROPERTY_VALUE), found != state.draft.advanced.end() && !state.busy);
}

void collect(HWND w, Dialog& state) {
  store_property(w, state);
  auto& fields = state.draft;
  fields.dsn = edit(w, IDC_DSN);
  fields.server = edit(w, IDC_SERVER);
  fields.port = edit(w, IDC_PORT);
  fields.database = edit(w, IDC_DATABASE);
  fields.user = edit(w, IDC_USER);
  secure_clear(fields.password);
  fields.password = edit(w, IDC_PASSWORD);
  fields.ssl = checked(w, IDC_SSL);
  if (checked(w, IDC_APP_DEFAULT)) inherit(fields, L"APPLICATIONNAME");
  else {
    fields.advanced[L"APPLICATIONNAME"] = edit(w, IDC_APP);
    fields.removed.erase(L"APPLICATIONNAME");
  }
  if (state.trust_touched) {
    const auto trust = SendDlgItemMessageW(w, IDC_TRUST, CB_GETCURSEL, 0, 0);
    inherit(fields, L"SSLCAFILE");
    inherit(fields, L"SSLCADIR");
    if (trust == 1 || trust == 2) {
      const auto key = trust == 1 ? L"SSLCAFILE" : L"SSLCADIR";
      const auto other = trust == 1 ? L"SSLCADIR" : L"SSLCAFILE";
      fields.advanced[key] = edit(w, IDC_CA_PATH);
      fields.advanced[other] = L""; // Override an inherited alternative trust source.
      fields.removed.erase(key);
      fields.removed.erase(other);
    }
  }
  validate(fields);
  if (!SQLValidDSNW(fields.dsn.c_str())) throw std::invalid_argument("Enter a valid DSN name.");
}

std::wstring diagnostic(SQLSMALLINT type, SQLHANDLE handle) {
  std::wstring result;
  for (SQLSMALLINT record = 1; record <= 8; ++record) {
    SQLWCHAR state[6]{};
    SQLINTEGER native = 0;
    SQLSMALLINT length = 0;
    // Raw server/driver text may echo credentials. Deliberately retrieve only
    // structured state/code and present fixed, actionable explanations.
    const auto rc = SQLGetDiagRecW(type, handle, record, state, &native, nullptr, 0, &length);
    if (!SQL_SUCCEEDED(rc)) break;
    const std::wstring code(reinterpret_cast<wchar_t*>(state), 5);
    result += L"SQLSTATE " + code + L"  |  Native code " + std::to_wstring(native) + L"\r\n";
    if (code == L"28000" || code == L"28P01")
      result += L"Authentication was rejected. Check the user name and test password.\r\n";
    else if (code == L"HYT00" || code == L"HYT01")
      result += L"The login deadline expired. Check the endpoint, port and network access.\r\n";
    else if (code == L"IM002" || code == L"IM003")
      result += L"The driver could not be loaded. Check its installation and application architecture.\r\n";
    else if (code.substr(0, 2) == L"08")
      result += L"The connection could not be established. Check the endpoint, network and TLS trust settings.\r\n";
    else result += L"The driver rejected the operation. Review the connection and advanced settings.\r\n";
  }
  return result.empty() ? L"No ODBC diagnostic record was available.\r\n" : result;
}

TestResult test_connection(const Fields& fields) {
  struct Handles {
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC dbc = SQL_NULL_HDBC;
    bool connected = false;
    ~Handles() {
      if (connected) SQLDisconnect(dbc);
      if (dbc) SQLFreeHandle(SQL_HANDLE_DBC, dbc);
      if (env) SQLFreeHandle(SQL_HANDLE_ENV, env);
    }
  } handles;
  TestResult result;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &handles.env))) {
    result.details = L"The ODBC environment could not be allocated.";
    return result;
  }
  if (!SQL_SUCCEEDED(SQLSetEnvAttr(handles.env, SQL_ATTR_ODBC_VERSION,
          reinterpret_cast<SQLPOINTER>(static_cast<UINT_PTR>(SQL_OV_ODBC3)), 0)) ||
      !SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, handles.env, &handles.dbc))) {
    result.details = diagnostic(SQL_HANDLE_ENV, handles.env);
    return result;
  }
  if (!SQL_SUCCEEDED(SQLSetConnectAttr(handles.dbc, SQL_ATTR_LOGIN_TIMEOUT,
                                      reinterpret_cast<SQLPOINTER>(static_cast<UINT_PTR>(5)), 0))) {
    result.details = diagnostic(SQL_HANDLE_DBC, handles.dbc);
    return result;
  }
  auto connection = connection_string(fields);
  struct Wipe { std::wstring& value; ~Wipe() { secure_clear(value); } } wipe{connection};
  const auto rc = SQLDriverConnectW(handles.dbc, nullptr,
      reinterpret_cast<SQLWCHAR*>(connection.data()), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
  handles.connected = SQL_SUCCEEDED(rc);
  result.succeeded = handles.connected;
  result.details = result.succeeded ? L"Connection succeeded.\r\n" : L"Connection failed.\r\n";
  if (!result.succeeded || rc == SQL_SUCCESS_WITH_INFO)
    result.details += diagnostic(SQL_HANDLE_DBC, handles.dbc);
  return result;
}

int page_for(int id) {
  switch (id) {
    case IDC_DSN: case IDC_DSN_LABEL: case IDC_SERVER: case IDC_SERVER_LABEL:
    case IDC_PORT: case IDC_PORT_LABEL: case IDC_DATABASE: case IDC_DATABASE_LABEL:
    case IDC_APP: case IDC_APP_LABEL: case IDC_APP_DEFAULT: case IDC_CONNECTION_NOTE: return 0;
    case IDC_USER: case IDC_USER_LABEL: case IDC_PASSWORD: case IDC_PASSWORD_LABEL:
    case IDC_PASSWORD_NOTE: case IDC_AUTH_LABEL: case IDC_SSL: case IDC_TRUST:
    case IDC_TRUST_LABEL: case IDC_CA_PATH: case IDC_CA_BROWSE: case IDC_TLS_NOTE: return 1;
    case IDC_PROPERTIES: case IDC_PROPERTIES_LABEL: case IDC_PROPERTY_OVERRIDE:
    case IDC_PROPERTY_VALUE: case IDC_PROPERTY_DESCRIPTION: return 2;
    case IDC_DETAILS: case IDC_DETAILS_LABEL: case IDC_TEST_NOTE: return 3;
    default: return -1;
  }
}
void show_page(HWND w, int page) {
  for (auto child = GetWindow(w, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
    const auto owned_page = page_for(GetDlgCtrlID(child));
    if (owned_page >= 0) ShowWindow(child, owned_page == page ? SW_SHOW : SW_HIDE);
  }
}
void enable_controls(HWND w, Dialog& state) {
  for (auto child = GetWindow(w, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
    const auto id = GetDlgCtrlID(child);
    if (page_for(id) >= 0 && id != IDC_DETAILS) EnableWindow(child, !state.busy);
  }
  EnableWindow(GetDlgItem(w, IDOK), !state.busy);
  EnableWindow(GetDlgItem(w, IDC_TEST), !state.busy);
  EnableWindow(GetDlgItem(w, IDC_DSN), !state.busy && !state.fixed_name);
  EnableWindow(GetDlgItem(w, IDC_APP), !state.busy && !checked(w, IDC_APP_DEFAULT));
  const bool custom = SendDlgItemMessageW(w, IDC_TRUST, CB_GETCURSEL, 0, 0) > 0;
  EnableWindow(GetDlgItem(w, IDC_CA_PATH), !state.busy && custom);
  EnableWindow(GetDlgItem(w, IDC_CA_BROWSE), !state.busy && custom);
  EnableWindow(GetDlgItem(w, IDC_PROPERTY_VALUE), !state.busy && checked(w, IDC_PROPERTY_OVERRIDE));
  text(w, IDC_TEST, state.busy ? L"Testing..." : L"Test connection");
}

void browse(HWND w) {
  const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  struct Uninit { HRESULT result; ~Uninit() { if (SUCCEEDED(result)) CoUninitialize(); } } uninit{initialized};
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return;
  IFileOpenDialog* picker = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&picker)))) return;
  struct Release { IFileOpenDialog* value; ~Release() { value->Release(); } } release{picker};
  FILEOPENDIALOGOPTIONS options = 0;
  picker->GetOptions(&options);
  const bool folder = SendDlgItemMessageW(w, IDC_TRUST, CB_GETCURSEL, 0, 0) == 2;
  picker->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                     (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
  picker->SetTitle(folder ? L"Select a CA certificate directory" : L"Select a CA certificate file");
  if (SUCCEEDED(picker->Show(w))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(picker->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        text(w, IDC_CA_PATH, path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
}

void set_heading_font(HWND w, Dialog& state) {
  LOGFONTW font{};
  font.lfHeight = -MulDiv(17, static_cast<int>(GetDpiForWindow(w)), 72);
  font.lfWeight = FW_SEMIBOLD;
  font.lfQuality = CLEARTYPE_QUALITY;
  wcscpy_s(font.lfFaceName, L"Segoe UI");
  auto replacement = CreateFontIndirectW(&font);
  if (!replacement) return;
  SendDlgItemMessageW(w, IDC_HEADING, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
  if (state.heading_font) DeleteObject(state.heading_font);
  state.heading_font = replacement;
}

void paint_navigation(HWND window, HDC dc) {
  RECT client{};
  GetClientRect(window, &client);
  FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
  const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
  const auto previous = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
  const auto selected = static_cast<int>(SendMessageW(window, TCM_GETCURSEL, 0, 0));
  const auto count = static_cast<int>(SendMessageW(window, TCM_GETITEMCOUNT, 0, 0));
  for (int index = 0; index < count; ++index) {
    RECT item{};
    SendMessageW(window, TCM_GETITEMRECT, index, reinterpret_cast<LPARAM>(&item));
    wchar_t label[64]{};
    TCITEMW tab{};
    tab.mask = TCIF_TEXT;
    tab.pszText = label;
    tab.cchTextMax = 64;
    SendMessageW(window, TCM_GETITEMW, index, reinterpret_cast<LPARAM>(&tab));
    DrawTextW(dc, label, -1, &item, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (index == selected) {
      RECT marker{item.left + 8, item.bottom - 3, item.right - 8, item.bottom};
      FillRect(dc, &marker, GetSysColorBrush(COLOR_HIGHLIGHT));
      if (GetFocus() == window) {
        InflateRect(&item, -4, -5);
        DrawFocusRect(dc, &item);
      }
    }
  }
  SelectObject(dc, previous);
}
void size_navigation(HWND window) {
  RECT client{};
  GetClientRect(window, &client);
  const auto count = static_cast<int>(SendMessageW(window, TCM_GETITEMCOUNT, 0, 0));
  if (count <= 0) return;
  int width = client.right / count;
  // The native button tab layout adds spacing outside the requested item width.
  // Measure it so all sections fit without an unexpected scroll-arrow control.
  for (int attempt = 0; attempt < 4; ++attempt) {
    SendMessageW(window, TCM_SETITEMSIZE, 0, MAKELPARAM(width, client.bottom - 2));
    RECT last{};
    SendMessageW(window, TCM_GETITEMRECT, count - 1, reinterpret_cast<LPARAM>(&last));
    if (SendMessageW(window, TCM_GETROWCOUNT, 0, 0) > 1 || last.bottom > client.bottom) {
      width -= std::max(4, static_cast<int>(client.right) / (count * 16));
      continue;
    }
    if (last.right <= client.right - 2) break;
    width -= (last.right - client.right + count + 1) / count + 1;
  }
}
LRESULT CALLBACK navigation_proc(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                  UINT_PTR id, DWORD_PTR) {
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    paint_navigation(window, dc);
    EndPaint(window, &paint);
    return 0;
  }
  if (message == WM_PRINTCLIENT) { paint_navigation(window, reinterpret_cast<HDC>(wp)); return 0; }
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, navigation_proc, id);
  const auto result = DefSubclassProc(window, message, wp, lp);
  if (message == WM_SIZE) size_navigation(window);
  if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_LBUTTONUP ||
      message == WM_KEYUP || message == TCM_SETCURSEL || message == WM_THEMECHANGED ||
      message == WM_SYSCOLORCHANGE) InvalidateRect(window, nullptr, FALSE);
  return result;
}

void initialize(HWND w, Dialog& state) {
  const auto title = driver_name() + L" Setup";
  SetWindowTextW(w, title.c_str());
  text(w, IDC_HEADING, from_utf8(rs::core::database::configured_backend_provider().identity().display_name));
  text(w, IDC_SCOPE, std::wstring(state.system_dsn ? L"System data source" : L"User data source") +
      (sizeof(void*) == 8 ? L"  |  64-bit driver" : L"  |  32-bit driver"));
  const auto& fields = state.draft;
  for (const auto& [id, value] : std::map<int, std::wstring>{{IDC_DSN, fields.dsn},
       {IDC_SERVER, fields.server}, {IDC_PORT, fields.port}, {IDC_DATABASE, fields.database},
       {IDC_USER, fields.user}, {IDC_PASSWORD, fields.password}}) {
    text(w, id, value);
    SendDlgItemMessageW(w, id, EM_SETLIMITTEXT, id == IDC_DSN ? 32 : 1024, 0);
  }
  for (auto id : {IDC_APP, IDC_CA_PATH}) SendDlgItemMessageW(w, id, EM_SETLIMITTEXT, 1024, 0);
  SendDlgItemMessageW(w, IDC_PROPERTY_VALUE, CB_LIMITTEXT, 1024, 0);
  CheckDlgButton(w, IDC_SSL, fields.ssl ? BST_CHECKED : BST_UNCHECKED);
  const auto application = fields.advanced.find(L"APPLICATIONNAME");
  CheckDlgButton(w, IDC_APP_DEFAULT, application == fields.advanced.end() ? BST_CHECKED : BST_UNCHECKED);
  if (application != fields.advanced.end()) text(w, IDC_APP, application->second);
  for (auto* value : {L"Driver default", L"CA certificate file", L"CA certificate directory"})
    SendDlgItemMessageW(w, IDC_TRUST, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
  const auto file = fields.advanced.find(L"SSLCAFILE");
  const auto directory = fields.advanced.find(L"SSLCADIR");
  const bool use_file = file != fields.advanced.end() &&
      (!file->second.empty() || directory == fields.advanced.end() || directory->second.empty());
  SendDlgItemMessageW(w, IDC_TRUST, CB_SETCURSEL,
      use_file ? 1 : directory != fields.advanced.end() ? 2 : 0, 0);
  if (use_file) text(w, IDC_CA_PATH, file->second);
  else if (directory != fields.advanced.end()) text(w, IDC_CA_PATH, directory->second);
  TCITEMW tab{};
  tab.mask = TCIF_TEXT;
  int index = 0;
  for (auto* label : {L"Connection", L"Security", L"Advanced", L"Diagnostics"}) {
    tab.pszText = const_cast<wchar_t*>(label);
    SendDlgItemMessageW(w, IDC_TABS, TCM_INSERTITEMW, index++, reinterpret_cast<LPARAM>(&tab));
  }
  SendDlgItemMessageW(w, IDC_TABS, TCM_SETPADDING, 0, MAKELPARAM(10, 5));
  RECT rect{};
  const auto navigation = GetDlgItem(w, IDC_TABS);
  if (!SetWindowSubclass(navigation, navigation_proc, 1, 0))
    throw std::runtime_error("Cannot initialize setup navigation.");
  size_navigation(navigation);
  const auto list = GetDlgItem(w, IDC_PROPERTIES);
  ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  GetClientRect(list, &rect);
  LVCOLUMNW column{};
  column.mask = LVCF_TEXT | LVCF_WIDTH;
  column.cx = (rect.right - rect.left) * 3 / 5;
  column.pszText = const_cast<wchar_t*>(L"Setting");
  SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
  column.cx = (rect.right - rect.left) * 2 / 5 - GetSystemMetrics(SM_CXVSCROLL);
  column.pszText = const_cast<wchar_t*>(L"Value");
  SendMessageW(list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
  state.updating = true;
  for (const auto& property : rs::odbc::setup::properties()) {
    if (property.key == L"APPLICATIONNAME" || property.key == L"SSLCAFILE" || property.key == L"SSLCADIR") continue;
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(state.properties.size());
    item.pszText = const_cast<wchar_t*>(property.label.c_str());
    SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    state.properties.push_back(property);
    state.selected_property = item.iItem;
    load_property(w, state, item.iItem);
    store_property(w, state);
  }
  state.updating = false;
  load_property(w, state, 0);
  ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  text(w, IDC_DETAILS, L"No connection test has been run.\r\n\r\nTests use the current draft, including advanced and retained settings.\r\nPasswords and raw server messages are excluded from these details.");
  set_heading_font(w, state);
  enable_controls(w, state);
  show_page(w, 0);
  state.trust_touched = false;
}

INT_PTR CALLBACK dialog_proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
  auto* state = reinterpret_cast<Dialog*>(GetWindowLongPtrW(w, DWLP_USER));
  try {
    if (message == WM_INITDIALOG) {
      state = reinterpret_cast<Dialog*>(lp);
      SetWindowLongPtrW(w, DWLP_USER, lp);
      initialize(w, *state);
      return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_CTLCOLORDLG) return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
    if (message == WM_CTLCOLORSTATIC) {
      const auto dc = reinterpret_cast<HDC>(wp);
      SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
      SetBkMode(dc, TRANSPARENT);
      return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
    }
    if (message == WM_DPICHANGED) {
      // Per-monitor-v2 dialog manager scales the controls and the dialog font.
      set_heading_font(w, *state);
      return FALSE;
    }
    if (message == WM_NOTIFY) {
      const auto* notification = reinterpret_cast<NMHDR*>(lp);
      if (notification->idFrom == IDC_TABS && notification->code == TCN_SELCHANGE) {
        show_page(w, static_cast<int>(SendDlgItemMessageW(w, IDC_TABS, TCM_GETCURSEL, 0, 0)));
        return TRUE;
      }
      if (notification->idFrom == IDC_PROPERTIES && notification->code == LVN_ITEMCHANGED && !state->updating) {
        const auto* changed = reinterpret_cast<NMLISTVIEW*>(lp);
        if ((changed->uNewState & LVIS_SELECTED) && !(changed->uOldState & LVIS_SELECTED)) {
          state->updating = true;
          store_property(w, *state);
          load_property(w, *state, changed->iItem);
          state->updating = false;
        }
        return TRUE;
      }
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
      if (state->busy) {
        state->close_when_finished = true;
        text(w, IDC_STATUS, L"Closing when the current test finishes...");
        EnableWindow(GetDlgItem(w, IDCANCEL), FALSE);
      } else {
        text(w, IDC_PASSWORD, L"");
        EndDialog(w, IDCANCEL);
      }
      return TRUE;
    }
    if (message == test_finished) {
      if (state->worker.joinable()) state->worker.join();
      state->busy = false;
      text(w, IDC_DETAILS, state->result.details);
      text(w, IDC_STATUS, state->result.succeeded ? L"Connection succeeded." : L"Connection failed. See Diagnostics for details.");
      enable_controls(w, *state);
      if (state->close_when_finished) {
        text(w, IDC_PASSWORD, L"");
        EndDialog(w, IDCANCEL);
      }
      return TRUE;
    }
    if (message == WM_COMMAND) {
      const auto id = LOWORD(wp);
      if ((id == IDC_TRUST && HIWORD(wp) == CBN_SELCHANGE) ||
          (id == IDC_CA_PATH && HIWORD(wp) == EN_CHANGE)) state->trust_touched = true;
      if (id == IDC_APP_DEFAULT || id == IDC_TRUST || id == IDC_PROPERTY_OVERRIDE) {
        enable_controls(w, *state);
        return TRUE;
      }
      if (id == IDC_CA_BROWSE && !state->busy) { browse(w); return TRUE; }
      if ((id == IDC_TEST || id == IDOK) && !state->busy) {
        collect(w, *state);
        if (id == IDOK) {
          secure_clear(state->fields->password);
          *state->fields = state->draft;
          secure_clear(state->fields->password);
          text(w, IDC_PASSWORD, L"");
          EndDialog(w, IDOK);
        } else {
          state->busy = true;
          enable_controls(w, *state);
          text(w, IDC_STATUS, L"Testing current settings...");
          state->worker = std::thread([w, state, draft = state->draft]() mutable {
            const auto started = std::chrono::steady_clock::now();
            try { state->result = state->tester ? state->tester(draft) : test_connection(draft); }
            catch (...) { state->result = {false, L"The connection test could not be completed."}; }
            secure_clear(draft.password);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            state->result.details += L"\r\nElapsed: " + std::to_wstring(elapsed) + L" ms. Requested login timeout: 5 seconds.";
            PostMessageW(w, test_finished, 0, 0);
          });
        }
        return TRUE;
      }
    }
  } catch (const std::exception& error) {
    if (state && !state->worker.joinable()) { state->busy = false; enable_controls(w, *state); }
    text(w, IDC_STATUS, L"Invalid settings. " + from_utf8(error.what()));
    return TRUE;
  } catch (...) {
    text(w, IDC_STATUS, L"The setup operation could not be completed.");
    return TRUE;
  }
  return FALSE;
}

struct PresentationContext {
  HANDLE activation = INVALID_HANDLE_VALUE;
  ULONG_PTR cookie = 0;
  using SetDpi = HANDLE(WINAPI*)(HANDLE);
  SetDpi set_dpi = nullptr;
  HANDLE previous_dpi = nullptr;
  explicit PresentationContext(HINSTANCE module) {
    ACTCTXW context{};
    context.cbSize = sizeof(context);
    context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    context.hModule = module;
    context.lpResourceName = MAKEINTRESOURCEW(2);
    activation = CreateActCtxW(&context);
    if (activation != INVALID_HANDLE_VALUE) ActivateActCtx(activation, &cookie);
    const auto user = GetModuleHandleW(L"user32.dll");
    set_dpi = reinterpret_cast<SetDpi>(GetProcAddress(user, "SetThreadDpiAwarenessContext"));
    if (set_dpi) previous_dpi = set_dpi(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))); // Per-monitor v2.
  }
  ~PresentationContext() {
    if (set_dpi && previous_dpi) set_dpi(previous_dpi);
    if (cookie) DeactivateActCtx(0, cookie);
    if (activation != INVALID_HANDLE_VALUE) ReleaseActCtx(activation);
  }
};
}  // namespace

bool edit_dialog(HINSTANCE module, HWND parent, Fields& fields, bool fixed_name,
                 bool system_dsn, ConnectionTester tester) {
  PresentationContext presentation(module);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES};
  if (!InitCommonControlsEx(&controls)) throw std::runtime_error("Cannot initialize setup controls.");
  Dialog state{&fields, fields, fixed_name, system_dsn};
  state.tester = std::move(tester);
  const auto result = DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_SETUP), parent,
                                     dialog_proc, reinterpret_cast<LPARAM>(&state));
  if (result == -1) throw std::runtime_error("Cannot open setup dialog.");
  return result == IDOK;
}
}  // namespace rs::odbc::setup
