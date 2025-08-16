#pragma once

#ifdef _WIN32
  #define NOMINMAX
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

#include <string>
#include <system_error>

namespace rs::platform {

#ifdef _WIN32
struct WSAInit {
  WSAInit() { WSADATA d{}; WSAStartup(MAKEWORD(2,2), &d); }
  ~WSAInit() { WSACleanup(); }
};
inline std::string last_error_text(const char* what) {
  int ec = WSAGetLastError();
  return std::string(what) + ": WSA error " + std::to_string(ec);
}
#else
struct WSAInit { WSAInit(){} ~WSAInit(){} };
inline std::string last_error_text(const char* what) {
  return std::string(what) + ": errno " + std::to_string(errno) + " (" + std::generic_category().message(errno) + ")";
}
#endif

} // namespace rs::platform