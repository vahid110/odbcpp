cmake_minimum_required(VERSION 3.20)
project(odbcpp VERSION 1.0.0 LANGUAGES CXX)

# ---- Project Options ----
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TESTING "Build tests" ON)
option(OPENSSL_USE_STATIC_LIBS "Link OpenSSL statically when available" OFF)

# ---- Global Settings ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Platform-specific Configuration ----
if(APPLE AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build architectures" FORCE)
endif()

# ---- Dependencies ----
include(cmake/FindDependencies.cmake)

# ---- Core Library ----
file(GLOB_RECURSE CORE_SOURCES 
  "core/*.cpp"
  "core/*.h"
)

add_library(${PROJECT_NAME}_core STATIC ${CORE_SOURCES})
add_library(${PROJECT_NAME}::core ALIAS ${PROJECT_NAME}_core)

target_include_directories(${PROJECT_NAME}_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(${PROJECT_NAME}_core 
  PUBLIC 
    OpenSSL::SSL 
    OpenSSL::Crypto
)

if(WIN32)
  target_link_libraries(${PROJECT_NAME}_core PRIVATE ws2_32 crypt32)
endif()

# ---- Compiler Settings ----
include(cmake/CompilerSettings.cmake)
apply_compiler_settings(${PROJECT_NAME}_core)

# ---- Examples ----
if(BUILD_EXAMPLES)
  include(cmake/Examples.cmake)
endif()

# ---- Tests ----
if(BUILD_TESTING)
  include(CTest)
  include(cmake/Tests.cmake)
endif()

# ---- Installation ----
include(cmake/Install.cmake)