// Supplemental Linux/unixODBC scanner context. Shared test sources otherwise
// infer the direct-driver branch and never include the manager-only header.
// Keep these defines local to this TU; the real header owns its guard and ABI
// assertion. This file is syntax-checked, never linked into a product or run.
#define ODBCPP_TEST_VIA_DRIVER_MANAGER 1
#define ODBCPP_EXPECT_DM_SQLWCHAR_SIZE 2
#include "../../tests/test_dm_odbc.h"
