// Narrow upstream backport regressions. Compile against the patched bundled
// headers, FMT_HEADER_ONLY=1 and the existing GoogleTest target; run under ASan
// in qualification. No subprocess, native store, database or network use.
#include <gtest/gtest.h>
#include <spdlog/fmt/bundled/printf.h>

#include <memory>

TEST(BundledFmtSecurity, TrailingPercentRejectsNonTerminatedView) {
  auto percent = std::make_unique<char>('%');
  EXPECT_THROW(fmt::sprintf(fmt::string_view(percent.get(), 1)), fmt::format_error);
  EXPECT_THROW(fmt::sprintf("hello%"), fmt::format_error);
  EXPECT_THROW(fmt::sprintf("%1$d%", 1, 2), fmt::format_error);
  EXPECT_EQ("before %", fmt::sprintf("before %%"));
  EXPECT_EQ("42", fmt::sprintf("%d", 42));
}

TEST(BundledFmtSecurity, EmptyHeaderDoesNotReadPastEnd) {
  auto character = std::make_unique<char>('x');
  const char* position = character.get() + 1;
  const char* end = position;
  auto specs = fmt::format_specs();
  EXPECT_EQ(-1, fmt::detail::parse_header(
                    position, end, specs,
                    [](int) { return fmt::basic_format_arg<fmt::printf_context>(); }));
  EXPECT_EQ(end, position);
}

TEST(BundledFmtSecurity, UnpackedArgumentsRejectNegativeIndex) {
  int value = 42;
  auto store = fmt::make_printf_args(value, value, value, value, value, value,
                                    value, value, value, value, value, value,
                                    value, value, value, value);
  auto arguments = fmt::basic_format_args<fmt::printf_context>(store);
  EXPECT_FALSE(static_cast<bool>(arguments.get(-1)));
  EXPECT_FALSE(static_cast<bool>(arguments.get(16)));
  EXPECT_TRUE(static_cast<bool>(arguments.get(0)));
  EXPECT_TRUE(static_cast<bool>(arguments.get(15)));
  EXPECT_THROW(fmt::sprintf("%*0$d", value, value, value, value, value, value,
                            value, value, value, value, value, value, value,
                            value, value, value), fmt::format_error);
  EXPECT_THROW(fmt::sprintf("%.*0$d", value, value, value, value, value, value,
                            value, value, value, value, value, value, value,
                            value, value, value), fmt::format_error);
}
