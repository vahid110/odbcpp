#pragma once

#ifdef ODBCPP_ENABLE_TEST_HOOKS
namespace rs::odbc::testing {

// Fails exactly one handle allocation inside SQLAllocHandle.
void fail_next_handle_allocation() noexcept;

}  // namespace rs::odbc::testing
#endif
