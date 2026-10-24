# Reviewed pinned AWS SDK C++ import (redshift, redshift-serverless, sts). Built
# separately with BUILD_ONLY=redshift;redshift-serverless;sts, USE_CRT_HTTP_CLIENT=ON,
# USE_CRT_HTTP_CLIENT_ASYNC_IO=OFF, USE_OPENSSL=ON, BUILD_SHARED_LIBS=OFF at version
# 1.11.906. This file is git-pinned; the SDK install prefix is a separate reviewed
# external dependency supplied via ODBCPP_AWS_SDK_INSTALL (not ambient discovery).
set(ODBCPP_AWS_SDK_INSTALL "" CACHE PATH "Reviewed AWS SDK install prefix (redshift, redshift-serverless, sts)")
set(_odbcpp_aws_config "${ODBCPP_AWS_SDK_INSTALL}/lib/cmake/AWSSDK/AWSSDKConfig.cmake")
if(NOT IS_ABSOLUTE "${ODBCPP_AWS_SDK_INSTALL}"
   OR NOT EXISTS "${_odbcpp_aws_config}"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/include/aws/sts/STSClient.h"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/include/aws/redshift/RedshiftClient.h"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/include/aws/redshift-serverless/RedshiftServerlessClient.h")
  message(FATAL_ERROR "Native provisioned auth requires an explicit existing ODBCPP_AWS_SDK_INSTALL with redshift+redshift-serverless+sts")
endif()

# Pin config-mode discovery to exactly the selected prefix. A cached/ambient
# AWSSDK_DIR that would resolve to a different installation is refused rather
# than silently overridden; the selected prefix is the only reviewed install.
set(_odbcpp_aws_config_dir "${ODBCPP_AWS_SDK_INSTALL}/lib/cmake/AWSSDK")
if(DEFINED AWSSDK_DIR AND NOT "${AWSSDK_DIR}" STREQUAL "" AND NOT "${AWSSDK_DIR}" STREQUAL "${_odbcpp_aws_config_dir}")
  message(FATAL_ERROR "Native provisioned auth refuses a cached AWSSDK_DIR (${AWSSDK_DIR}) that differs from the selected prefix (${_odbcpp_aws_config_dir})")
endif()
set(AWSSDK_DIR "${_odbcpp_aws_config_dir}" CACHE PATH "Reviewed AWS SDK config directory" FORCE)
set(AWSSDK_ROOT_DIR "${ODBCPP_AWS_SDK_INSTALL}" CACHE PATH "Reviewed AWS SDK root" FORCE)
# The selected prefix leads the search path so the SDK's own internal component
# dependencies (aws-cpp-sdk-core, CRT, crypto) resolve from the same install.
list(PREPEND CMAKE_PREFIX_PATH "${ODBCPP_AWS_SDK_INSTALL}")

find_package(ZLIB REQUIRED)
# Exact reviewed version. Never fall back to an ambient or unrelated config:
# AWSSDK_DIR selects the selected prefix's config directly.
find_package(AWSSDK 1.11.906 EXACT CONFIG REQUIRED COMPONENTS redshift redshift-serverless sts)

if(NOT DEFINED AWSSDK_VERSION OR NOT AWSSDK_VERSION VERSION_EQUAL "1.11.906")
  message(FATAL_ERROR "Native provisioned auth requires reviewed AWS SDK 1.11.906 (found ${AWSSDK_VERSION})")
endif()
if(AWSSDK_INSTALL_AS_SHARED_LIBS)
  message(FATAL_ERROR "Native provisioned auth requires the reviewed static (BUILD_SHARED_LIBS=OFF) AWS SDK")
endif()

foreach(_target aws-cpp-sdk-redshift aws-cpp-sdk-redshift-serverless aws-cpp-sdk-sts)
  if(NOT TARGET ${_target})
    message(FATAL_ERROR "Native provisioned auth incomplete AWS SDK import closure (missing ${_target})")
  endif()
  get_target_property(_odbcpp_aws_type ${_target} TYPE)
  if(NOT _odbcpp_aws_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Native provisioned auth requires static imported SDK libraries (${_target} is ${_odbcpp_aws_type})")
  endif()
  # The imported archive must resolve inside the selected prefix; an ambient
  # component config that points at another installation is refused.
  get_target_property(_odbcpp_aws_loc ${_target} IMPORTED_LOCATION_RELEASE)
  if(NOT _odbcpp_aws_loc)
    get_target_property(_odbcpp_aws_loc ${_target} IMPORTED_LOCATION)
  endif()
  if(NOT _odbcpp_aws_loc OR NOT IS_ABSOLUTE "${_odbcpp_aws_loc}")
    message(FATAL_ERROR "Native provisioned auth cannot verify the imported location of ${_target}")
  endif()
  file(REAL_PATH "${_odbcpp_aws_loc}" _odbcpp_aws_loc_real)
  if(NOT _odbcpp_aws_loc_real MATCHES "^${ODBCPP_AWS_SDK_INSTALL}/")
    message(FATAL_ERROR "Native provisioned auth imported ${_target} from outside the selected prefix (${_odbcpp_aws_loc_real})")
  endif()
endforeach()

set(SDK_FIXTURE_INCLUDES "${ODBCPP_AWS_SDK_INSTALL}/include")
