#define NOMINMAX
#include <windows.h>
#include <string>
#include <cstdio>
int wmain(int argc,wchar_t** argv) {
  if(argc!=2)return 2;
  const std::wstring directory=argv[1];
  for(const auto file:{L"odbcpp.dll",L"odbcpp_setup.dll"}) {
    const auto path=directory+L"\\"+file;
    if(!LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32)) {
      std::fprintf(stderr,"Packaged DLL load failed: %lu\n",GetLastError());return 1;
    }
  }
  for(const auto file:{L"libssl-3-x64.dll",L"libcrypto-3-x64.dll",L"msvcp140.dll",L"vcruntime140.dll"}) {
    const auto module=GetModuleHandleW(file);wchar_t path[32768]{};
    if(!module || !GetModuleFileNameW(module,path,32768) ||
       _wcsicmp(path,(directory+L"\\"+file).c_str())!=0) {
      std::fprintf(stderr,"Runtime dependency did not load from the package directory.\n");return 1;
    }
  }
  std::puts("Packaged driver/setup and app-local runtime dependencies loaded successfully.");return 0;
}
