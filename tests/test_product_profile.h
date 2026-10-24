#pragma once

// Whether the compiled product is Redshift, for fixture SQL that differs by
// product. Direct builds ask the linked core; Driver Manager builds cannot link
// it and use the same CMake TARGET_DATABASE choice as a compile definition.
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
#ifndef ODBCPP_TEST_REDSHIFT_PRODUCT
#error "Driver Manager suite builds must define ODBCPP_TEST_REDSHIFT_PRODUCT"
#endif
#else
#include "core/database/database_factory.h"
#endif

namespace odbcpp::test {

inline bool compiled_redshift_product() {
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
  return ODBCPP_TEST_REDSHIFT_PRODUCT != 0;
#else
  return rs::core::database::DatabaseFactory::get_compiled_database_type() ==
      rs::core::database::DatabaseType::Redshift;
#endif
}

}  // namespace odbcpp::test
