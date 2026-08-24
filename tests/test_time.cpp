#include <gtest/gtest.h>

#include "wasafe/core/time.hpp"

using namespace WaSafe;

// TimeRange: пересечение и вложенность
TEST(Time, RangeIntersectAndHull) {
    TimeRange const a{100, 200};
    EXPECT_EQ(a.size(), 100u);
    EXPECT_TRUE(a.contains(150));
    EXPECT_FALSE(a.contains(200));  // полуоткрытый интервал

    TimeRange const b{150, 300};
    EXPECT_TRUE(a.overlaps(b));
    EXPECT_EQ(a.intersect(b), (TimeRange{150, 200}));
    EXPECT_EQ(a.hull(b), (TimeRange{100, 300}));
}

// TimeRange: пустые интервалы
TEST(Time, EmptyRanges) {
    EXPECT_TRUE((TimeRange{5, 5}.empty()));
    EXPECT_TRUE((TimeRange{9, 5}.empty()));
    EXPECT_FALSE((TimeRange{5, 9}.empty()));
}

// масштаб в секунды
TEST(Time, ToSeconds) {
    // 1000 единиц при масштабе 1 ns = 1e-6 c
    const TimeScale ns{.exponent = static_cast<int>(TimeUnit::NS), .scale = 1};
    EXPECT_NEAR(toSeconds(1000, ns), 1e-6, 1e-18);
}
