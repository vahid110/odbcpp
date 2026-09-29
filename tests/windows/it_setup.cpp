#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <sql.h>
#include <odbcinst.h>
#include "../../odbc/setup/resource.h"
#include <string>
#include <thread>
#include <atomic>
#include <cstdio>

namespace {
constexpr auto driver=L"ODBCPP PostgreSQL";
constexpr auto name=L"ODBCPP_Setup_\u6d4b\u8bd5";
std::wstring attrs(std::initializer_list<std::wstring> values) {
  std::wstring result;for(const auto& v:values){result+=v;result+=L'\0';}result+=L'\0';return result;
}
std::wstring read(const wchar_t* section,const wchar_t* key) {
  wchar_t buffer[2048]{};SQLGetPrivateProfileStringW(section,key,L"<absent>",buffer,2048,L"ODBC.INI");return buffer;
}
bool check(bool ok,const char* what) {if(!ok) std::fprintf(stderr,"Setup acceptance failed: %s\n",what);return ok;}
HWND find_dialog(DWORD thread) {
  HWND found=nullptr;
  EnumThreadWindows(thread,[](HWND window,LPARAM data)->BOOL {
    wchar_t title[100]{};GetWindowTextW(window,title,100);
    if(std::wstring(title)==L"ODBCPP PostgreSQL Setup") {*reinterpret_cast<HWND*>(data)=window;return FALSE;}return TRUE;
  },reinterpret_cast<LPARAM>(&found));return found;
}
bool capture(HWND window,const wchar_t* name) {
  RECT rect{};GetWindowRect(window,&rect);const int width=rect.right-rect.left,height=rect.bottom-rect.top;
  BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
  info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
  HDC dc=GetDC(window),memory=CreateCompatibleDC(dc);void* pixels=nullptr;
  HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
  if(!bitmap || !memory) {if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);ReleaseDC(window,dc);return false;}
  const auto old=SelectObject(memory,bitmap);bool ok=PrintWindow(window,memory,2)!=FALSE;
  wchar_t temp[MAX_PATH]{};
  if(!GetEnvironmentVariableW(L"RUNNER_TEMP",temp,MAX_PATH)) GetTempPathW(MAX_PATH,temp);
  const auto path=std::wstring(temp)+L"\\"+name;
  HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
  header.bfSize=header.bfOffBits+width*height*4;DWORD written=0;
  if(file==INVALID_HANDLE_VALUE)ok=false;
  else {
    ok=WriteFile(file,&header,sizeof(header),&written,nullptr)&&ok;
    ok=WriteFile(file,&info.bmiHeader,sizeof(BITMAPINFOHEADER),&written,nullptr)&&ok;
    ok=WriteFile(file,pixels,width*height*4,&written,nullptr)&&ok;CloseHandle(file);
  }
  SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(window,dc);return ok;
}
bool dialog(HWND parent,const std::wstring& input,bool cancel,bool add=false) {
  const auto thread=GetCurrentThreadId();std::atomic<bool> passed=false;
  std::thread automation([&] {
    HWND w=nullptr;for(int i=0;i<200 && !w;++i){Sleep(50);w=find_dialog(thread);}
    if(!w)return;
    const auto click=[&](int id){DWORD_PTR ignored=0;return SendMessageTimeoutW(w,WM_COMMAND,MAKEWPARAM(id,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(w,id)),SMTO_ABORTIFHUNG,15000,&ignored)!=0;};
    const auto status=[&] {wchar_t value[256]{};GetDlgItemTextW(w,IDC_STATUS,value,256);return std::wstring(value);};
    bool ok=check(!IsWindowEnabled(GetDlgItem(w,IDC_DSN)),"DSN must remain fixed") &&
      check((GetWindowLongPtrW(GetDlgItem(w,IDC_PASSWORD),GWL_STYLE)&ES_PASSWORD)!=0,"masked password") &&
      check(SendDlgItemMessageW(w,IDC_TABS,TCM_GETITEMCOUNT,0,0)==2,"extensible connection/auth tabs");
    ok=check(capture(w,L"odbcpp-setup-connection.bmp"),"connection screenshot")&&ok;
    SetDlgItemTextW(w,IDC_PORT,L"5432");
    if(cancel) {passed=ok;click(IDCANCEL);return;}
    SetDlgItemTextW(w,IDC_PORT,L"0");click(IDOK);
    ok=check(status().find(L"Invalid settings")!=std::wstring::npos,"invalid port stays in dialog")&&ok;
    SetDlgItemTextW(w,IDC_PORT,L"5432");
    SendDlgItemMessageW(w,IDC_TABS,TCM_SETCURSEL,1,0);
    NMHDR notify{GetDlgItem(w,IDC_TABS),IDC_TABS,TCN_SELCHANGE};SendMessageW(w,WM_NOTIFY,IDC_TABS,reinterpret_cast<LPARAM>(&notify));
    ok=check(IsWindowVisible(GetDlgItem(w,IDC_PASSWORD))&&!IsWindowVisible(GetDlgItem(w,IDC_SERVER)),"authentication tab selection")&&ok;
    SetDlgItemTextW(w,IDC_PASSWORD,L"wrong-password");
    ok=click(IDC_TEST)&&check(status().find(L"Connection failed")!=std::wstring::npos,"failed login feedback")&&ok;
    SetDlgItemTextW(w,IDC_PASSWORD,L"postgres");
    ok=click(IDC_TEST)&&check(status()==L"Connection succeeded.","successful connection test")&&ok;
    ok=check(capture(w,L"odbcpp-setup-authentication.bmp"),"authentication screenshot")&&ok;
    passed=ok;click(ok?IDOK:IDCANCEL);
  });
  const bool result=SQLConfigDataSourceW(parent,add?ODBC_ADD_DSN:ODBC_CONFIG_DSN,driver,input.c_str())!=FALSE;
  automation.join();return passed && result==!cancel;
}
}
int main() {
  const auto dsn=std::wstring(L"DSN=")+name;
  const auto initial=attrs({dsn,L"SERVER=127.0.0.1",L"PORT=5432",L"DATABASE=postgres",L"UID=postgres",L"PWD=postgres",L"SSL=0"});
  const auto selected=attrs({dsn});
  // Dedicated fixture names only; never replace an existing entry.
  SQLSetConfigMode(ODBC_USER_DSN);
  if(!check(read(L"ODBC Data Sources",name)==L"<absent>","clean fixture"))return 1;
  bool ok=check(SQLConfigDataSourceW(nullptr,ODBC_ADD_DSN,driver,initial.c_str())!=FALSE,"Unicode add through installer");
  SQLSetConfigMode(ODBC_USER_DSN);
  ok=check(read(name,L"PWD")==L"<absent>" && read(name,L"UID")==L"postgres","no persisted password")&&ok;
  ok=check(!SQLConfigDataSourceW(nullptr,ODBC_ADD_DSN,driver,initial.c_str()),"duplicate add rejected")&&ok;
  const auto edit=attrs({dsn,L"PORT=5444"});
  ok=check(SQLConfigDataSourceW(nullptr,ODBC_CONFIG_DSN,driver,edit.c_str())!=FALSE,"partial edit")&&ok;
  SQLSetConfigMode(ODBC_USER_DSN);
  ok=check(read(name,L"Port")==L"5444" && read(name,L"Server")==L"127.0.0.1","edit preserves omitted fields")&&ok;
  const auto bad=attrs({dsn,L"PORT=65536"});
  ok=check(!SQLConfigDataSourceW(nullptr,ODBC_CONFIG_DSN,driver,bad.c_str()),"invalid silent edit")&&ok;
  SQLSetConfigMode(ODBC_USER_DSN);
  ok=check(read(name,L"Port")==L"5444","failed validation does not persist")&&ok;
  HWND parent=CreateWindowExW(0,L"STATIC",L"ODBCPP setup acceptance",WS_OVERLAPPED,0,0,400,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  ok=check(parent!=nullptr,"parent window")&&ok;
  if(parent && ok) {
    const auto canceled_add=attrs({L"DSN=ODBCPP_Setup_Canceled",L"SERVER=127.0.0.1",L"DATABASE=postgres"});
    ok=check(dialog(parent,canceled_add,true,true),"cancel add")&&ok;
    SQLSetConfigMode(ODBC_USER_DSN);
    ok=check(read(L"ODBC Data Sources",L"ODBCPP_Setup_Canceled")==L"<absent>","canceled add creates no DSN")&&ok;
    ok=check(dialog(parent,selected,true),"cancel")&&ok;
    SQLSetConfigMode(ODBC_USER_DSN);ok=check(read(name,L"Port")==L"5444","cancel preserves registry")&&ok;
    ok=check(dialog(parent,selected,false),"edit/test/save dialog")&&ok;
    SQLSetConfigMode(ODBC_USER_DSN);
    ok=check(read(name,L"Port")==L"5432" && read(name,L"PWD")==L"<absent>","save settings without test password")&&ok;
  }
  if(parent)DestroyWindow(parent);
  SQLSetConfigMode(ODBC_USER_DSN);
  SQLWritePrivateProfileStringW(L"ODBC Data Sources",name,L"Other Driver",L"ODBC.INI");
  ok=check(!SQLConfigDataSourceW(nullptr,ODBC_REMOVE_DSN,driver,selected.c_str()),"foreign DSN protected")&&ok;
  SQLSetConfigMode(ODBC_USER_DSN);
  ok=check(read(name,L"Server")==L"127.0.0.1","foreign contents retained")&&ok;
  SQLWritePrivateProfileStringW(L"ODBC Data Sources",name,driver,L"ODBC.INI");
  ok=check(SQLConfigDataSourceW(nullptr,ODBC_ADD_SYS_DSN,driver,initial.c_str())!=FALSE,"System DSN add")&&ok;
  ok=check(SQLConfigDataSourceW(nullptr,ODBC_REMOVE_DSN,driver,selected.c_str())!=FALSE,"User DSN removal")&&ok;
  SQLSetConfigMode(ODBC_SYSTEM_DSN);
  ok=check(read(name,L"Server")==L"127.0.0.1","same-name System DSN preserved")&&ok;
  ok=check(SQLConfigDataSourceW(nullptr,ODBC_REMOVE_SYS_DSN,driver,selected.c_str())!=FALSE,"System DSN removal")&&ok;
  SQLSetConfigMode(ODBC_USER_DSN);
  SQLWritePrivateProfileStringW(L"ODBCPP_Setup_Orphan",L"Unrelated",L"keep",L"ODBC.INI");
  const auto orphan=attrs({L"DSN=ODBCPP_Setup_Orphan",L"SERVER=127.0.0.1",L"DATABASE=postgres"});
  ok=check(!SQLConfigDataSourceW(nullptr,ODBC_ADD_DSN,driver,orphan.c_str()),"orphan section protected")&&ok;
  SQLSetConfigMode(ODBC_USER_DSN);
  ok=check(read(L"ODBCPP_Setup_Orphan",L"Unrelated")==L"keep","orphan content retained")&&ok;
  SQLWritePrivateProfileStringW(L"ODBCPP_Setup_Orphan",nullptr,nullptr,L"ODBC.INI");
  const char ansi[]="DSN=ODBCPP_Setup_ANSI\0SERVER=127.0.0.1\0DATABASE=postgres\0SSL=0\0";
  ok=check(SQLConfigDataSource(nullptr,ODBC_ADD_DSN,"ODBCPP PostgreSQL",ansi)!=FALSE,"ANSI entry point")&&ok;
  ok=check(SQLConfigDataSource(nullptr,ODBC_REMOVE_DSN,"ODBCPP PostgreSQL","DSN=ODBCPP_Setup_ANSI\0")!=FALSE,"ANSI removal")&&ok;
  SQLSetConfigMode(ODBC_BOTH_DSN);
  if(ok)std::puts("Native setup acceptance passed: add/edit/cancel/validation/login/removal and scope isolation.");
  return ok?0:1;
}
