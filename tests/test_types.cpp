#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "wasafe/types/type.hpp"

using namespace WaSafe;

// ширина вектора
TEST(Types, VectorWidth) {
    auto v = makeVector(7, 0);
    ASSERT_EQ(v->kind(), TypeKind::VECTOR);
    EXPECT_EQ(v->bitWidth(), 8u);

    auto v2 = makeVector(0, 31);  // обратный порядок битов
    EXPECT_EQ(v2->bitWidth(), 32u);
}

// packed-структура: ширина и поиск члена
TEST(Types, PackedStructWidthAndLookup) {
    auto s = makeStruct(
            {
                    StructMember{"addr", makeVector(7, 0), 1},
                    StructMember{"valid", makeScalar(), 0},
            },
            /*packed=*/true);

    const auto* st = s->as<StructType>();
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->elementCount(), 2u);
    EXPECT_EQ(st->bitWidth(), 9u);  // 8 + 1
    EXPECT_EQ(st->indexOf("valid"), 1u);
    EXPECT_FALSE(st->indexOf("missing").has_value());
}

// многомерный массив выражается вложением
TEST(Types, MultiDimArrayNesting) {
    // logic[3:0] mem [0:1][0:1]
    auto inner = makeArray(makeVector(3, 0), 0, 1);
    auto mem = makeArray(inner, 0, 1);

    const auto* outer = mem->as<ArrayType>();
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->elementCount(), 2u);
    ASSERT_EQ(outer->elementType()->kind(), TypeKind::ARRAY);
    EXPECT_EQ(outer->elementType()->elementCount(), 2u);
    EXPECT_EQ(outer->ordinalOf(1), 1u);
}

// Индекс вне диапазона массива не даёт мусорный порядковый номер.
TEST(Types, ArrayIndexOutOfRange) {
    auto asc = makeArray(makeScalar(), 0, 3);  // [0:3]
    const auto* up = asc->as<ArrayType>();
    ASSERT_NE(up, nullptr);
    EXPECT_EQ(up->elementCount(), 4u);
    EXPECT_EQ(up->ordinalOf(0), 0u);
    EXPECT_EQ(up->ordinalOf(3), 3u);
    EXPECT_FALSE(up->ordinalOf(-1).has_value());
    EXPECT_FALSE(up->ordinalOf(4).has_value());

    auto desc = makeArray(makeScalar(), 3, 0);  // [3:0]: ord 0 — это indexLeft
    const auto* down = desc->as<ArrayType>();
    ASSERT_NE(down, nullptr);
    EXPECT_EQ(down->ordinalOf(3), 0u);
    EXPECT_EQ(down->ordinalOf(0), 3u);
    EXPECT_FALSE(down->ordinalOf(4).has_value());
    EXPECT_FALSE(down->ordinalOf(-1).has_value());
}

// Порядковый номер вне числа элементов не даёт мусорный индекс.
TEST(Types, ArrayOrdinalOutOfRange) {
    auto t = makeArray(makeScalar(), 3, 0);
    const auto* at = t->as<ArrayType>();
    ASSERT_NE(at, nullptr);
    EXPECT_EQ(at->indexOf(0), 3);
    EXPECT_EQ(at->indexOf(3), 0);
    EXPECT_FALSE(at->indexOf(4).has_value());
    EXPECT_FALSE(at->indexOf(std::numeric_limits<std::size_t>::max()).has_value());
}

// Края int32: разность краёв не должна переполнять знаковую арифметику.
TEST(Types, ArrayExtremeIndexBounds) {
    constexpr std::int32_t kLo = std::numeric_limits<std::int32_t>::min();
    constexpr std::int32_t kHi = std::numeric_limits<std::int32_t>::max();
    constexpr std::size_t kCount = std::size_t{1} << 32;

    auto asc = makeArray(makeScalar(), kLo, kHi);
    const auto* up = asc->as<ArrayType>();
    ASSERT_NE(up, nullptr);
    EXPECT_EQ(up->elementCount(), kCount);
    EXPECT_EQ(up->ordinalOf(kLo), 0u);
    EXPECT_EQ(up->ordinalOf(0), std::size_t{1} << 31);
    EXPECT_EQ(up->ordinalOf(kHi), kCount - 1);

    auto desc = makeArray(makeScalar(), kHi, kLo);
    const auto* down = desc->as<ArrayType>();
    ASSERT_NE(down, nullptr);
    EXPECT_EQ(down->elementCount(), kCount);
    EXPECT_EQ(down->ordinalOf(kHi), 0u);
    EXPECT_EQ(down->ordinalOf(kLo), kCount - 1);
    EXPECT_EQ(down->indexOf(kCount - 1), kLo);
}

// indexOf и ordinalOf обратны друг другу на всём диапазоне, включая ноль внутри.
TEST(Types, ArrayIndexOrdinalRoundTrip) {
    auto t = makeArray(makeScalar(), 7, -8);
    const auto* at = t->as<ArrayType>();
    ASSERT_NE(at, nullptr);
    ASSERT_EQ(at->elementCount(), 16u);

    for (std::size_t k = 0; k < at->elementCount(); ++k) {
        const auto index = at->indexOf(k);
        ASSERT_TRUE(index.has_value()) << "ordinal " << k;
        EXPECT_EQ(at->ordinalOf(*index), k);
    }
}

// Отсутствующая метка перечисления отличима от метки с пустым именем.
TEST(Types, EnumLabelLookup) {
    const Type t = std::make_shared<const EnumType>(makeVector(1, 0),
            std::vector<EnumEntry>{{"IDLE", 0}, {"RUN", 1}, {"", 2}});

    const auto* et = t->as<EnumType>();
    ASSERT_NE(et, nullptr);
    EXPECT_EQ(et->labelOf(1), "RUN");
    EXPECT_EQ(et->labelOf(2), "");  // метка есть, но пустая
    EXPECT_FALSE(et->labelOf(7).has_value());
}
