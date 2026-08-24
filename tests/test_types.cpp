#include <gtest/gtest.h>

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
    EXPECT_EQ(st->indexOf("missing"), static_cast<std::size_t>(-1));
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
