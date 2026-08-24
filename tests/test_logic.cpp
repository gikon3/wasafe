#include <gtest/gtest.h>

#include "wasafe/core/exception.hpp"
#include "wasafe/types/value.hpp"

using namespace WaSafe;

// dynamic_bitset-подобный API: размер, мутаторы, сравнение
TEST(Logic, DynamicBitsetApi) {
    LogicVector v;
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0u);

    v.resize(4);  // fill = 0 по умолчанию
    EXPECT_EQ(v.size(), 4u);
    EXPECT_EQ(v.toString(), "0000");

    v.set();  // все -> 1
    EXPECT_EQ(v.toString(), "1111");
    v.reset();  // все -> 0
    EXPECT_EQ(v.toString(), "0000");

    v.set(0);                         // дефолт -> 1
    v.set(2, Logic::X);               // явное значение
    v.flip(0);                        // 1 -> 0
    v.flip(1);                        // 0 -> 1
    EXPECT_EQ(v.toString(), "0x10");  // MSB..LSB: [3]=0 [2]=x [1]=1 [0]=0
    EXPECT_EQ(v.test(1), Logic::ONE);
    v.reset(1);
    EXPECT_EQ(v.test(1), Logic::ZERO);

    // проверка границ
    EXPECT_THROW((void)v.test(4), Exception);
    EXPECT_THROW(v.flip(4), Exception);

    // push_back / pop_back растят и сжимают
    LogicVector w;
    w.pushBack(Logic::ONE);
    w.pushBack(Logic::ZERO);
    w.pushBack(Logic::X);
    EXPECT_EQ(w.toString(), "x01");  // MSB..LSB: [2]=x [1]=0 [0]=1
    w.popBack();
    EXPECT_EQ(w.toString(), "01");
    EXPECT_EQ(w.size(), 2u);

    // равенство по ширине и обоим бит-планам
    LogicVector a(2);
    a.set(0, Logic::ONE);
    EXPECT_EQ(a, w);
    a.flip(0);
    EXPECT_NE(a, w);

    w.clear();
    EXPECT_TRUE(w.empty());

    // прокси: flip() и operator~
    LogicVector p(1);
    p[0] = Logic::ONE;
    p[0].flip();
    EXPECT_EQ(p.test(0), Logic::ZERO);
    EXPECT_EQ(~p[0], Logic::ONE);  // operator~ не мутирует
    EXPECT_EQ(p.test(0), Logic::ZERO);
}

// LogicVector set/get отдельных бит
TEST(Logic, SetGetBits) {
    LogicVector v(8);
    v.set(0, Logic::ONE);
    v.set(1, Logic::ZERO);
    v.set(7, Logic::X);
    EXPECT_EQ(v.get(0), Logic::ONE);
    EXPECT_EQ(v.get(1), Logic::ZERO);
    EXPECT_EQ(v.get(7), Logic::X);
}

// set() — проверяемая запись: индекс вне диапазона бросает исключение
TEST(Logic, SetOutOfRangeThrows) {
    LogicVector v(8);
    EXPECT_THROW(v.set(8, Logic::ONE), Exception);
    EXPECT_THROW(v.set(100, Logic::X), Exception);
    EXPECT_NO_THROW(v.set(7, Logic::ONE));  // граница допустима
}

// Прокси-запись через operator[]: vec[i] = Logic, чтение обоих бит-планов
TEST(Logic, ProxyWriteSubscript) {
    LogicVector v(4);
    v[0] = Logic::ONE;   // (a,b) = (1,0)
    v[1] = Logic::Z;     // (a,b) = (0,1)
    v[2] = Logic::X;     // (a,b) = (1,1)
    v[3] = Logic::ZERO;  // (a,b) = (0,0)

    EXPECT_EQ(v[0], Logic::ONE);
    EXPECT_EQ(v[1], Logic::Z);
    EXPECT_EQ(v[2], Logic::X);
    EXPECT_EQ(v[3], Logic::ZERO);
    EXPECT_EQ(v.toString(), "0xz1");  // MSB(3)..LSB(0)

    // Перезапись: бит может менять состояние во всех направлениях.
    v[2] = Logic::ONE;
    EXPECT_EQ(v[2], Logic::ONE);

    // Присваивание из другого прокси пишет значение, а не переуказывает ссылку.
    v[3] = v[0];
    EXPECT_EQ(v[3], Logic::ONE);
}

// LogicVectorView::to_string печатает MSB..LSB
TEST(Logic, ToStringMsbFirst) {
    LogicVector v(4);
    v.assignFromChars("10xz");  // MSB=1 ... LSB=z
    EXPECT_EQ(v.toString(), "10xz");
    EXPECT_FALSE(v.isTwoState());
}

// LogicVectorView::to_uint64 только для двухзначных
TEST(Logic, ToUint64TwoStateOnly) {
    LogicVector v(8);
    v.assignFromUint64(0b1010'0101);
    auto out = v.toUint64();
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, 0xA5u);

    v.set(3, Logic::Z);
    out = v.toUint64();
    EXPECT_FALSE(out);  // появилось Z => не двухзначно
}

// Двухзначная форма вьюхи: bval == nullptr означает «все b-биты нулевые».
TEST(Logic, TwoStateViewWithoutBvalPlane) {
    // 0b1011 в одном слове; bval-плана нет вовсе.
    const std::uint64_t aval = 0b1011;
    const LogicVectorView v{&aval, nullptr, 4};

    EXPECT_EQ(v.width(), 4u);
    EXPECT_TRUE(v.isTwoState());
    EXPECT_TRUE(v.bval().empty());  // плана нет => пустой span, а не UB
    EXPECT_EQ(v.toString(), "1011");
    EXPECT_EQ(v.toUint64(), 0b1011u);

    EXPECT_EQ(v[0], Logic::ONE);
    EXPECT_EQ(v[1], Logic::ONE);
    EXPECT_EQ(v[2], Logic::ZERO);
    EXPECT_EQ(v[3], Logic::ONE);

    // Копирование в владеющий вектор разворачивает нулевой bval-план.
    const LogicVector owned{v};
    EXPECT_EQ(owned.width(), 4u);
    EXPECT_EQ(owned.toString(), "1011");
    EXPECT_TRUE(owned.isTwoState());
    EXPECT_EQ(owned.bval().size(), LogicVectorView::wordsFor(4));
    EXPECT_EQ(owned, LogicVector{v});  // инварианты не разъехались
}

// ширина в словах
TEST(Logic, WordsForWidth) {
    EXPECT_EQ(LogicVectorView::wordsFor(1), 1u);
    EXPECT_EQ(LogicVectorView::wordsFor(64), 1u);
    EXPECT_EQ(LogicVectorView::wordsFor(65), 2u);
}
