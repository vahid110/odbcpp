#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <sql.h>
#include <sqlext.h>
#include <odbcinst.h>
#include "setup_dialog.h"
#include "resource.h"
namespace rs::odbc::setup {
namespace {
struct Dialog { Fields* fields; bool fixed_name; };
std::wstring edit(HWND window,int id) {
  const auto control=GetDlgItem(window,id);
  const int size=GetWindowTextLengthW(control);
  std::wstring value(static_cast<std::size_t>(size)+1,L'\0');
  GetWindowTextW(control,value.data(),size+1); value.resize(size); return value;
}
void collect(HWND w,Fields& f) {
  f.dsn=edit(w,IDC_DSN);f.server=edit(w,IDC_SERVER);f.port=edit(w,IDC_PORT);
  f.database=edit(w,IDC_DATABASE);f.user=edit(w,IDC_USER);f.password=edit(w,IDC_PASSWORD);
  f.ssl=IsDlgButtonChecked(w,IDC_SSL)==BST_CHECKED;
  validate(f);
  if(!SQLValidDSNW(f.dsn.c_str()) || f.dsn==L"ODBC Data Sources") throw std::invalid_argument("Invalid DSN name.");
}
bool test_connection(const Fields& f) {
  SQLHENV env=SQL_NULL_HENV;SQLHDBC dbc=SQL_NULL_HDBC;
  bool ok=SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV,SQL_NULL_HANDLE,&env));
  if(ok) ok=SQL_SUCCEEDED(SQLSetEnvAttr(env,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),0));
  if(ok) ok=SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC,env,&dbc));
  if(ok) ok=SQL_SUCCEEDED(SQLSetConnectAttr(dbc,SQL_ATTR_LOGIN_TIMEOUT,reinterpret_cast<SQLPOINTER>(5),0));
  auto connection=connection_string(f);
  if(ok) ok=SQL_SUCCEEDED(SQLDriverConnectW(dbc,nullptr,reinterpret_cast<SQLWCHAR*>(connection.data()),SQL_NTS,nullptr,0,nullptr,SQL_DRIVER_NOPROMPT));
  SecureZeroMemory(connection.data(),connection.size()*sizeof(wchar_t));
  if(ok) SQLDisconnect(dbc);
  if(dbc) SQLFreeHandle(SQL_HANDLE_DBC,dbc);
  if(env) SQLFreeHandle(SQL_HANDLE_ENV,env);
  return ok;
}
void show_page(HWND w,int page) {
  for(auto id:{IDC_DSN,IDC_DSN_LABEL,IDC_SERVER,IDC_SERVER_LABEL,IDC_PORT,IDC_PORT_LABEL,
               IDC_DATABASE,IDC_DATABASE_LABEL,IDC_SSL}) ShowWindow(GetDlgItem(w,id),page==0?SW_SHOW:SW_HIDE);
  for(auto id:{IDC_USER,IDC_USER_LABEL,IDC_PASSWORD,IDC_PASSWORD_LABEL,IDC_PASSWORD_NOTE,IDC_AUTH_LABEL})
    ShowWindow(GetDlgItem(w,id),page==1?SW_SHOW:SW_HIDE);
}
INT_PTR CALLBACK dialog_proc(HWND w,UINT message,WPARAM wp,LPARAM lp) {
  auto* state=reinterpret_cast<Dialog*>(GetWindowLongPtrW(w,DWLP_USER));
  if(message==WM_INITDIALOG) {
    state=reinterpret_cast<Dialog*>(lp);SetWindowLongPtrW(w,DWLP_USER,lp);
    const auto& f=*state->fields;
    for(const auto& [id,value]:std::map<int,std::wstring>{{IDC_DSN,f.dsn},{IDC_SERVER,f.server},{IDC_PORT,f.port},{IDC_DATABASE,f.database},{IDC_USER,f.user},{IDC_PASSWORD,f.password}}) {
      SetDlgItemTextW(w,id,value.c_str());SendDlgItemMessageW(w,id,EM_SETLIMITTEXT,id==IDC_DSN?32:1024,0);
    }
    TCITEMW item{};item.mask=TCIF_TEXT;
    item.pszText=const_cast<wchar_t*>(L"Connection");SendDlgItemMessageW(w,IDC_TABS,TCM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item));
    item.pszText=const_cast<wchar_t*>(L"Authentication");SendDlgItemMessageW(w,IDC_TABS,TCM_INSERTITEMW,1,reinterpret_cast<LPARAM>(&item));
    show_page(w,0);
    EnableWindow(GetDlgItem(w,IDC_DSN),!state->fixed_name);
    CheckDlgButton(w,IDC_SSL,f.ssl?BST_CHECKED:BST_UNCHECKED);return TRUE;
  }
  if(message==WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->idFrom==IDC_TABS && reinterpret_cast<NMHDR*>(lp)->code==TCN_SELCHANGE) {
    show_page(w,static_cast<int>(SendDlgItemMessageW(w,IDC_TABS,TCM_GETCURSEL,0,0)));return TRUE;
  }
  if(message==WM_CLOSE || (message==WM_COMMAND && LOWORD(wp)==IDCANCEL)) {SetDlgItemTextW(w,IDC_PASSWORD,L"");EndDialog(w,IDCANCEL);return TRUE;}
  if(message==WM_COMMAND && (LOWORD(wp)==IDOK || LOWORD(wp)==IDC_TEST)) {
    try {
      Fields draft=*state->fields;collect(w,draft);
      if(LOWORD(wp)==IDC_TEST) {
        SetDlgItemTextW(w,IDC_STATUS,L"Testing connection...");
        SetDlgItemTextW(w,IDC_STATUS,test_connection(draft)?L"Connection succeeded.":L"Connection failed. Check settings and credentials.");
      } else {
        *state->fields=draft;SetDlgItemTextW(w,IDC_PASSWORD,L"");EndDialog(w,IDOK);
      }
    } catch(const std::exception&) {SetDlgItemTextW(w,IDC_STATUS,L"Invalid settings. Check name, server, database and port (1-65535).");}
    return TRUE;
  }
  return FALSE;
}
}
bool edit_dialog(HINSTANCE module, HWND parent, Fields& fields, bool fixed_name) {
  INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TAB_CLASSES};
  if(!InitCommonControlsEx(&controls)) throw std::runtime_error("Cannot initialize setup controls.");
  Dialog state{&fields,fixed_name};
  const auto result=DialogBoxParamW(module,MAKEINTRESOURCEW(IDD_SETUP),parent,dialog_proc,reinterpret_cast<LPARAM>(&state));
  if(result==-1) throw std::runtime_error("Cannot open setup dialog.");
  return result==IDOK;
}
}
