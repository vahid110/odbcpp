// tests/unit/test_datarow.cpp
#include <gtest/gtest.h>
#include <string>
#include <vector>

// Minimal DataRow parser matching production logic (text mode)
static std::vector<std::string> parse_datarow(const std::vector<unsigned char>& payload) {
  if (payload.size() < 2) throw std::runtime_error("DataRow too short");
  uint16_t ncols = (uint16_t)((payload[0] << 8) | payload[1]);
  size_t off = 2, n = payload.size();
  std::vector<std::string> row; row.reserve(ncols);
  for (uint16_t i = 0; i < ncols; ++i) {
    if (off + 4 > n) throw std::runtime_error("DataRow column header truncated");
    int32_t clen = (int32_t)((payload[off] << 24) | (payload[off+1] << 16)
                           | (payload[off+2] << 8) | payload[off+3]);
    off += 4;
    if (clen < 0) {
      row.emplace_back(); // NULL → empty string in our current API
    } else {
      if (off + (size_t)clen > n) throw std::runtime_error("DataRow column truncated");
      row.emplace_back(reinterpret_cast<const char*>(&payload[off]), (size_t)clen);
      off += (size_t)clen;
    }
  }
  return row;
}

TEST(DataRow, SingleColumnText) {
  // ncols=1, len=3, "foo"
  std::vector<unsigned char> pl = {
    0x00, 0x01,              // 1 column
    0x00, 0x00, 0x00, 0x03,  // length=3
    'f','o','o'
  };
  auto row = parse_datarow(pl);
  ASSERT_EQ(row.size(), 1u);
  EXPECT_EQ(row[0], "foo");
}

TEST(DataRow, MultipleColumnsWithNull) {
  // ncols=3:  "hello", NULL, "xyz"
  std::vector<unsigned char> pl = {
    0x00, 0x03,              // 3 columns
    0x00,0x00,0x00,0x05, 'h','e','l','l','o',
    0xFF,0xFF,0xFF,0xFF,     // -1 = NULL
    0x00,0x00,0x00,0x03, 'x','y','z'
  };
  auto row = parse_datarow(pl);
  ASSERT_EQ(row.size(), 3u);
  EXPECT_EQ(row[0], "hello");
  EXPECT_TRUE(row[1].empty()); // our API uses empty string to represent NULL at this layer
  EXPECT_EQ(row[2], "xyz");
}

TEST(DataRow, TruncatedThrows) {
  // Bad payload: claims 1 column but no length available
  std::vector<unsigned char> pl = { 0x00, 0x01 };
  EXPECT_THROW(parse_datarow(pl), std::runtime_error);
}
