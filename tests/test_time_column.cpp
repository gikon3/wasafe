#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "wasafe/types/time_column.hpp"

using namespace WaSafe;

namespace {

/// Порог, за которым узкое (32-битное) представление больше не годится.
constexpr TimeStamp kNarrowLimit = 0xFFFF'FFFF;

}  // namespace

// Пустая колонка не падает и не находит ничего.
TEST(TimeColumn, Empty) {
    const TimeColumn c;

    EXPECT_TRUE(c.empty());
    EXPECT_EQ(c.size(), 0u);
    EXPECT_EQ(c.front(), kNoTime);
    EXPECT_EQ(c.back(), kNoTime);
    EXPECT_EQ(c[0], kNoTime);
    EXPECT_EQ(c.upperBound(0), 0u);
    EXPECT_EQ(c.lowerBound(0), 0u);
    EXPECT_EQ(c.byteSize(), 0u);
}

// Узкое представление: доступ и поиск.
TEST(TimeColumn, NarrowAccessAndSearch) {
    TimeColumn c;
    for (const TimeStamp t : {10, 20, 30, 40})
        c.append(t);

    EXPECT_EQ(c.size(), 4u);
    EXPECT_EQ(c.front(), 10);
    EXPECT_EQ(c.back(), 40);
    EXPECT_EQ(c[0], 10);
    EXPECT_EQ(c[3], 40);
    EXPECT_EQ(c[4], kNoTime);  // вне границ

    // upperBound — первый строго больший.
    EXPECT_EQ(c.upperBound(5), 0u);
    EXPECT_EQ(c.upperBound(10), 1u);  // ровно на метке
    EXPECT_EQ(c.upperBound(15), 1u);
    EXPECT_EQ(c.upperBound(40), 4u);
    EXPECT_EQ(c.upperBound(99), 4u);

    // lowerBound — первый не меньший.
    EXPECT_EQ(c.lowerBound(5), 0u);
    EXPECT_EQ(c.lowerBound(10), 0u);  // ровно на метке
    EXPECT_EQ(c.lowerBound(15), 1u);
    EXPECT_EQ(c.lowerBound(40), 3u);
    EXPECT_EQ(c.lowerBound(99), 4u);

    // Узкая форма — четыре байта на метку.
    EXPECT_EQ(c.byteSize(), 4u * sizeof(std::uint32_t));
}

// «Повышение» до абсолютных меток: значения сохраняются, поиск продолжает работать.
TEST(TimeColumn, PromotesOnWideGap) {
    TimeColumn c;
    c.append(0);
    c.append(100);
    ASSERT_EQ(c.byteSize(), 2u * sizeof(std::uint32_t));  // пока узкая

    const TimeStamp far = kNarrowLimit + 1;  // не влезает в смещение uint32
    c.append(far);
    c.append(far + 5);

    EXPECT_EQ(c.byteSize(), 4u * sizeof(TimeStamp));  // повысилась

    // Ранее записанные метки не пострадали.
    EXPECT_EQ(c.size(), 4u);
    EXPECT_EQ(c[0], 0);
    EXPECT_EQ(c[1], 100);
    EXPECT_EQ(c[2], far);
    EXPECT_EQ(c[3], far + 5);
    EXPECT_EQ(c.front(), 0);
    EXPECT_EQ(c.back(), far + 5);

    EXPECT_EQ(c.upperBound(100), 2u);
    EXPECT_EQ(c.lowerBound(far), 2u);
    EXPECT_EQ(c.upperBound(far + 5), 4u);
}

// Ровно на границе узкого представления повышения быть не должно.
TEST(TimeColumn, StaysNarrowAtLimit) {
    TimeColumn c;
    c.append(1000);
    c.append(1000 + kNarrowLimit);  // максимальное допустимое смещение

    EXPECT_EQ(c.byteSize(), 2u * sizeof(std::uint32_t));
    EXPECT_EQ(c[1], 1000 + kNarrowLimit);
    EXPECT_EQ(c.lowerBound(1000 + kNarrowLimit), 1u);
}

// Ловушка переполнения: поиск получает границы kWholeTime (INT64_MIN/MAX),
// где прямое вычитание t - base переполнило бы int64.
TEST(TimeColumn, SearchWithExtremeBounds) {
    constexpr TimeStamp kMin = std::numeric_limits<TimeStamp>::min();
    constexpr TimeStamp kMax = std::numeric_limits<TimeStamp>::max();

    TimeColumn narrow;
    for (const TimeStamp t : {10, 20, 30})
        narrow.append(t);

    EXPECT_EQ(narrow.lowerBound(kMin), 0u);  // всё правее
    EXPECT_EQ(narrow.upperBound(kMin), 0u);
    EXPECT_EQ(narrow.lowerBound(kMax), 3u);  // всё левее
    EXPECT_EQ(narrow.upperBound(kMax), 3u);

    // То же самое на повышенной колонке.
    TimeColumn wide;
    wide.append(0);
    wide.append(kNarrowLimit + 100);
    EXPECT_EQ(wide.lowerBound(kMin), 0u);
    EXPECT_EQ(wide.upperBound(kMax), 2u);
}

// Отрицательные метки: колонка их принимает и корректно ищет.
TEST(TimeColumn, NegativeTimestamps) {
    TimeColumn c;
    for (const TimeStamp t : {-50, -10, 0, 10})
        c.append(t);

    EXPECT_EQ(c.front(), -50);
    EXPECT_EQ(c.back(), 10);
    EXPECT_EQ(c[1], -10);
    EXPECT_EQ(c.upperBound(-10), 2u);
    EXPECT_EQ(c.lowerBound(-50), 0u);
    EXPECT_EQ(c.lowerBound(-49), 1u);
}

// Срез: содержимое совпадает, представление выбирается заново.
TEST(TimeColumn, Slice) {
    TimeColumn c;
    for (const TimeStamp t : {5, 15, 25, 35, 45})
        c.append(t);

    const TimeColumn mid = c.slice(1, 4);
    ASSERT_EQ(mid.size(), 3u);
    EXPECT_EQ(mid[0], 15);
    EXPECT_EQ(mid[2], 35);
    EXPECT_EQ(mid.upperBound(15), 1u);

    // Срез повышенной колонки может снова стать узким.
    TimeColumn wide;
    wide.append(0);
    wide.append(kNarrowLimit + 1);
    wide.append(kNarrowLimit + 2);
    const TimeColumn tail = wide.slice(1, 3);
    ASSERT_EQ(tail.size(), 2u);
    EXPECT_EQ(tail[0], kNarrowLimit + 1);
    EXPECT_EQ(tail[1], kNarrowLimit + 2);
    EXPECT_EQ(tail.byteSize(), 2u * sizeof(std::uint32_t));

    // Границы среза зажимаются по размеру.
    EXPECT_EQ(c.slice(3, 99).size(), 2u);
    EXPECT_EQ(c.slice(99, 99).size(), 0u);
}

// Повторяющиеся метки (время не убывает, но может не расти).
TEST(TimeColumn, RepeatedTimestamps) {
    TimeColumn c;
    for (const TimeStamp t : {10, 10, 20})
        c.append(t);

    EXPECT_EQ(c.size(), 3u);
    EXPECT_EQ(c.lowerBound(10), 0u);
    EXPECT_EQ(c.upperBound(10), 2u);
}
