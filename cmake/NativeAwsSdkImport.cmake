# Reviewed pinned AWS SDK C++ import (redshift, redshift-serverless, sts,
# sso-oidc). Built separately with BUILD_ONLY=redshift;redshift-serverless;sts;sso-oidc,
# USE_CRT_HTTP_CLIENT=ON, USE_CRT_HTTP_CLIENT_ASYNC_IO=OFF, USE_OPENSSL=ON,
# BUILD_SHARED_LIBS=OFF at version 1.11.906. This file is git-pinned; the SDK
# install prefix is a separate reviewed external dependency supplied via
# ODBCPP_AWS_SDK_INSTALL (not ambient discovery).
set(ODBCPP_AWS_SDK_INSTALL "" CACHE PATH "Reviewed AWS SDK install prefix (redshift, redshift-serverless, sts, sso-oidc)")
set(_odbcpp_aws_config "${ODBCPP_AWS_SDK_INSTALL}/lib/cmake/AWSSDK/AWSSDKConfig.cmake")
if(NOT IS_ABSOLUTE "${ODBCPP_AWS_SDK_INSTALL}"
   OR NOT EXISTS "${_odbcpp_aws_config}"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/include/aws/sts/STSClient.h"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/include/aws/redshift/RedshiftClient.h"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/include/aws/redshift-serverless/RedshiftServerlessClient.h"
   OR NOT EXISTS "${ODBCPP_AWS_SDK_INSTALL}/include/aws/sso-oidc/SSOOIDCClient.h")
  message(FATAL_ERROR "Native provisioned auth requires an explicit existing ODBCPP_AWS_SDK_INSTALL with redshift+redshift-serverless+sts+sso-oidc")
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
find_package(AWSSDK 1.11.906 EXACT CONFIG REQUIRED COMPONENTS redshift redshift-serverless sts sso-oidc)

if(NOT DEFINED AWSSDK_VERSION OR NOT AWSSDK_VERSION VERSION_EQUAL "1.11.906")
  message(FATAL_ERROR "Native provisioned auth requires reviewed AWS SDK 1.11.906 (found ${AWSSDK_VERSION})")
endif()
if(AWSSDK_INSTALL_AS_SHARED_LIBS)
  message(FATAL_ERROR "Native provisioned auth requires the reviewed static (BUILD_SHARED_LIBS=OFF) AWS SDK")
endif()

# Validate the SDK/core/CRT link closure: every selectable imported archive must
# be static and resolve inside the selected prefix (canonical path containment,
# not a prefix regex). External reviewed crypto/zlib dependencies are handled by
# their own pinned discovery, not this closure.
function(_odbcpp_validate_imported _target _required)
  if(NOT TARGET ${_target})
    if(_required)
      message(FATAL_ERROR "Native provisioned auth incomplete AWS SDK import closure (missing ${_target})")
    endif()
    return()
  endif()
  get_target_property(_odbcpp_aws_type ${_target} TYPE)
  if(NOT _odbcpp_aws_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Native provisioned auth requires static imported SDK libraries (${_target} is ${_odbcpp_aws_type})")
  endif()
  # Verify every selectable configuration's archive, not only Release/unqualified.
  set(_odbcpp_aws_locs "")
  get_target_property(_odbcpp_aws_configs ${_target} IMPORTED_CONFIGURATIONS)
  if(_odbcpp_aws_configs)
    foreach(_cfg IN LISTS _odbcpp_aws_configs)
      get_target_property(_odbcpp_aws_loc ${_target} IMPORTED_LOCATION_${_cfg})
      if(_odbcpp_aws_loc)
        list(APPEND _odbcpp_aws_locs "${_odbcpp_aws_loc}")
      endif()
    endforeach()
  endif()
  get_target_property(_odbcpp_aws_loc ${_target} IMPORTED_LOCATION)
  if(_odbcpp_aws_loc)
    list(APPEND _odbcpp_aws_locs "${_odbcpp_aws_loc}")
  endif()
  list(REMOVE_DUPLICATES _odbcpp_aws_locs)
  if(NOT _odbcpp_aws_locs)
    message(FATAL_ERROR "Native provisioned auth cannot verify imported locations of ${_target}")
  endif()
  foreach(_odbcpp_aws_loc IN LISTS _odbcpp_aws_locs)
    if(NOT IS_ABSOLUTE "${_odbcpp_aws_loc}")
      message(FATAL_ERROR "Native provisioned auth imported ${_target} from a non-absolute location (${_odbcpp_aws_loc})")
    endif()
    # Resolve symlinks (canonical path) before enforcing containment; a lexical
    # prefix check would accept an archive symlinked outside the selected prefix.
    file(REAL_PATH "${_odbcpp_aws_loc}" _odbcpp_aws_loc_real)
    file(RELATIVE_PATH _odbcpp_aws_rel "${_odbcpp_aws_prefix_real}" "${_odbcpp_aws_loc_real}")
    if(IS_ABSOLUTE "${_odbcpp_aws_rel}" OR _odbcpp_aws_rel MATCHES "^\\.\\.(/|$)")
      message(FATAL_ERROR "Native provisioned auth imported ${_target} from outside the selected prefix (${_odbcpp_aws_loc_real})")
    endif()
  endforeach()
endfunction()
file(REAL_PATH "${ODBCPP_AWS_SDK_INSTALL}" _odbcpp_aws_prefix_real)
foreach(_target IN ITEMS aws-cpp-sdk-redshift aws-cpp-sdk-redshift-serverless aws-cpp-sdk-sts
    aws-cpp-sdk-sso-oidc aws-cpp-sdk-core AWS::aws-crt-cpp)
  _odbcpp_validate_imported(${_target} TRUE)
endforeach()
foreach(_target IN ITEMS AWS::aws-c-http AWS::aws-c-io AWS::aws-c-auth AWS::aws-c-cal
    AWS::aws-c-common AWS::aws-c-compression AWS::aws-c-event-stream AWS::aws-c-mqtt
    AWS::aws-c-s3 AWS::aws-c-sdkutils AWS::aws-checksums)
  _odbcpp_validate_imported(${_target} FALSE)
endforeach()

set(SDK_FIXTURE_INCLUDES "${ODBCPP_AWS_SDK_INSTALL}/include")
