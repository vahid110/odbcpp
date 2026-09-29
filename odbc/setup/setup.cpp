#define NOMINMAX
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <odbcinst.h>
#include "../setup_model.h"
#include "setup_dialog.h"
#include <mutex>
#include <vector>
#include <optional>
#include <cwctype>
#include <cwchar>
#include <cstring>

namespace {
using namespace rs::odbc::setup;
HINSTANCE module;
std::recursive_mutex mutex;
constexpr auto ini = L"ODBC.INI";
const wchar_t* keys[]{L"Server",L"Port",L"Database",L"UID",L"SSL",L"PWD",L"PASSWORD"};
std::wstring upper(std::wstring s) { for(auto& c:s)c=std::towupper(c); return s; }
Attributes attributes(const wchar_t* input) {
  Attributes result;
  if(!input) return result;
  for(const auto* p=input; *p; p+=std::wcslen(p)+1) {
    std::wstring entry(p);
    const auto eq=entry.find(L'=');
    if(eq==std::wstring::npos) throw std::invalid_argument("Invalid setup attribute.");
    const auto key=upper(entry.substr(0,eq));
    if(key!=L"DSN" && key!=L"SERVER" && key!=L"PORT" && key!=L"DATABASE" &&
       key!=L"UID" && key!=L"PWD" && key!=L"SSL") throw std::invalid_argument("Unsupported setup attribute.");
    if(!result.emplace(key,entry.substr(eq+1)).second) throw std::invalid_argument("Duplicate setup attribute.");
  }
  return result;
}
std::optional<std::wstring> read(const std::wstring& section, const wchar_t* key) {
  constexpr auto missing=L"\x01";
  wchar_t value[32768]{};
  const auto size=SQLGetPrivateProfileStringW(section.c_str(),key,missing,value,32768,ini);
  if(size>=32767) throw std::runtime_error("Stored setting is too long.");
  if(std::wstring(value)==missing) return std::nullopt;
  return std::wstring(value);
}
void overlay(Fields& f,const Attributes& a) {
  for(const auto& [key,value]:a) {
    if(key==L"DSN") f.dsn=value;
    else if(key==L"SERVER") f.server=value;
    else if(key==L"PORT") f.port=value;
    else if(key==L"DATABASE") f.database=value;
    else if(key==L"UID") f.user=value;
    else if(key==L"PWD") f.password=value;
    else if(key==L"SSL") {
      const auto v=upper(value);
      if(v==L"1"||v==L"ON"||v==L"TRUE"||v==L"YES") f.ssl=true;
      else if(v==L"0"||v==L"OFF"||v==L"FALSE"||v==L"NO") f.ssl=false;
      else throw std::invalid_argument("Invalid TLS setting.");
    }
  }
}
bool save(const Fields& f,bool add) {
  const auto values=persisted(f);
  std::map<std::wstring,std::optional<std::wstring>> before;
  if(!add) for(auto key:keys) before[key]=read(f.dsn,key);
  if(add && !SQLWriteDSNToIniW(f.dsn.c_str(),driver_name)) return false;
  bool ok=true;
  for(const auto& [key,value]:values) if(!SQLWritePrivateProfileStringW(f.dsn.c_str(),key.c_str(),value.c_str(),ini)) {ok=false;break;}
  if(ok) for(auto key:{L"PWD",L"PASSWORD"}) if(!SQLWritePrivateProfileStringW(f.dsn.c_str(),key,nullptr,ini)) {ok=false;break;}
  if(!ok) {
    if(add) SQLRemoveDSNFromIniW(f.dsn.c_str());
    else for(const auto& [key,value]:before) SQLWritePrivateProfileStringW(f.dsn.c_str(),key.c_str(),value?value->c_str():nullptr,ini);
  }
  return ok;
}
BOOL configure(HWND parent,WORD request,const wchar_t* driver,const wchar_t* input) {
  std::lock_guard lock(mutex);
  UWORD original=ODBC_BOTH_DSN;
  if(!SQLGetConfigMode(&original)) return FALSE;
  struct Restore { UWORD mode; ~Restore(){SQLSetConfigMode(mode);} } restore{original};
  auto mode=original==ODBC_SYSTEM_DSN?ODBC_SYSTEM_DSN:ODBC_USER_DSN;
  if(request>=ODBC_ADD_SYS_DSN && request<=ODBC_REMOVE_SYS_DSN) {mode=ODBC_SYSTEM_DSN;request-=ODBC_ADD_SYS_DSN-ODBC_ADD_DSN;}
  try {
    if(!driver || std::wstring(driver)!=driver_name || request<ODBC_ADD_DSN || request>ODBC_REMOVE_DSN)
      throw std::invalid_argument("Unsupported setup request or driver.");
    if(!SQLSetConfigMode(mode)) throw std::runtime_error("Cannot select DSN scope.");
    const auto a=attributes(input);
    Fields f;overlay(f,a);
    const bool add=request==ODBC_ADD_DSN;
    const bool fixed_name=!f.dsn.empty();
    if(fixed_name || !add) {
      validate_name(f.dsn);
      if(!SQLValidDSNW(f.dsn.c_str())) throw std::invalid_argument("Invalid DSN name.");
    }
    // Verify ownership before reading, updating or removing a named DSN.
    auto owner=f.dsn.empty()?std::nullopt:read(L"ODBC Data Sources",f.dsn.c_str());
    if((add && owner) || (!add && (!owner || *owner!=driver_name)))
      throw std::invalid_argument("DSN already exists or does not belong to this driver.");
    if(!add) {
      if(request==ODBC_REMOVE_DSN) return SQLRemoveDSNFromIniW(f.dsn.c_str());
      Attributes stored;
      for(auto key:keys) if(std::wstring(key)!=L"PWD" && std::wstring(key)!=L"PASSWORD") {
        if(auto value=read(f.dsn,key)) stored[upper(key)]=*value;
      }
      overlay(f,stored);overlay(f,a);
    }
    if(parent) {
      if(!rs::odbc::setup::edit_dialog(module,parent,f,fixed_name)) return FALSE;
    }
    validate(f);
    if(!SQLValidDSNW(f.dsn.c_str()) || upper(f.dsn)==L"ODBC DATA SOURCES") throw std::invalid_argument("Invalid DSN name.");
    // The Add dialog can supply the name; recheck before creating it.
    if(add) {
      wchar_t existing[4]{};
      if(read(L"ODBC Data Sources",f.dsn.c_str()) ||
         SQLGetPrivateProfileStringW(f.dsn.c_str(),nullptr,L"",existing,4,ini)>0)
        throw std::invalid_argument("DSN already exists.");
    }
    if(!save(f,add)) throw std::runtime_error("Cannot save DSN. Check registry permissions.");
    return TRUE;
  } catch(const std::exception&) {
    SQLPostInstallerErrorW(ODBC_ERROR_REQUEST_FAILED,L"ODBCPP setup failed. Check settings, DSN ownership and registry permissions.");
    if(parent) MessageBoxW(parent,L"Could not save the data source. Check its name and registry permissions.",L"ODBCPP PostgreSQL Setup",MB_OK|MB_ICONERROR);
    return FALSE;
  }
}
std::wstring ansi(const char* value) {
  if(!value) return {};
  const int size=MultiByteToWideChar(CP_ACP,0,value,-1,nullptr,0);
  if(!size) throw std::invalid_argument("Invalid ANSI setup input.");
  std::wstring result(size,L'\0');MultiByteToWideChar(CP_ACP,0,value,-1,result.data(),size);result.pop_back();return result;
}
}
extern "C" BOOL INSTAPI ConfigDSNW(HWND parent,WORD request,LPCWSTR driver,LPCWSTR input) { return configure(parent,request,driver,input); }
extern "C" BOOL INSTAPI ConfigDSN(HWND parent,WORD request,LPCSTR driver,LPCSTR input) {
  try {
    std::wstring converted;
    if(input) for(auto p=input;*p;p+=std::strlen(p)+1) {converted+=ansi(p);converted+=L'\0';}
    converted+=L'\0';
    return configure(parent,request,ansi(driver).c_str(),converted.c_str());
  } catch(...) {return FALSE;}
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {if(reason==DLL_PROCESS_ATTACH) module=instance;return TRUE;}
