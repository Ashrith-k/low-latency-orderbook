#include <gtest/gtest.h>

// Smoke test: proves the GoogleTest + CTest wiring compiles, links, is
// discovered, and runs. Real module specs replace this starting with the
// NaiveBook tests (Day 1, task 13).
TEST(Smoke, FrameworkRuns) { EXPECT_EQ(1 + 1, 2); }
