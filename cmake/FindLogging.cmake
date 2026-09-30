# ---- Logging ----
# Use spdlog's header-only distribution so the installed static ODBCPP target
# does not expose a third-party link dependency.
include(FetchContent)
set(ODBCPP_SPDLOG_VERSION "1.17.0")
set(ODBCPP_SPDLOG_ARCHIVE_SHA256 "b11912a82d149792fef33fabd0503b13d54aeac25c1464755461d4108ea71fc2")
FetchContent_Declare(
  spdlog
  URL https://github.com/gabime/spdlog/archive/refs/tags/v${ODBCPP_SPDLOG_VERSION}.zip
  URL_HASH SHA256=${ODBCPP_SPDLOG_ARCHIVE_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(spdlog)
if(NOT spdlog_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(spdlog)
  if(POLICY CMP0169)
    cmake_policy(POP)
  endif()
endif()
set(SPDLOG_INCLUDE_DIR "${spdlog_SOURCE_DIR}/include")
