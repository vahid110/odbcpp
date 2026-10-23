# Reviewed pinned AWS SDK C++ import (redshift + sts). Built separately with
# BUILD_ONLY=redshift;sts, USE_CRT_HTTP_CLIENT=ON, USE_CRT_HTTP_CLIENT_ASYNC_IO=OFF,
# USE_OPENSSL=ON, BUILD_SHARED_LIBS=OFF at version 1.11.906. This file is
# git-pinned; the SDK install prefix is a separate reviewed external dependency
# supplied via ODBCPP_AWS_SDK_INSTALL (not ambient discovery).
set(ODBCPP_AWS_SDK_INSTALL "" CACHE PATH "Reviewed AWS SDK install prefix (redshift + sts)")
if(NOT IS_ABSOLUTE "${ODBCPP_AWS_SDK_INSTALL}"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/lib/cmake/AWSSDK/AWSSDKConfig.cmake"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/include/aws/sts/STSClient.h"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/include/aws/redshift/RedshiftClient.h")
  message(FATAL_ERROR "Native provisioned auth requires an explicit existing ODBCPP_AWS_SDK_INSTALL with redshift+sts")
endif()
set(AWSSDK_ROOT_DIR "${ODBCPP_AWS_SDK_INSTALL}")
list(PREPEND CMAKE_PREFIX_PATH "${ODBCPP_AWS_SDK_INSTALL}")
find_package(AWSSDK CONFIG REQUIRED COMPONENTS redshift sts)
if(NOT TARGET aws-cpp-sdk-redshift OR NOT TARGET aws-cpp-sdk-sts)
  message(FATAL_ERROR "Native provisioned auth incomplete AWS SDK import closure")
endif()
set(SDK_FIXTURE_INCLUDES "${ODBCPP_AWS_SDK_INSTALL}/include")
