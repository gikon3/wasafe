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

// --- Форматирование по основанию -------------------------------------------
// Группировка бит: 1/3/4 бита на разряд, старший разряд слева. Число разрядов
// определяется ШИРИНОЙ, а не значением: ведущие нули не срезаются — так же, как
// двоичный toString() всегда печатает ровно width() символов.
TEST(Logic, RadixGroupsByWidthNotValue) {
    LogicVector v(4);
    v.assignFromChars("1010");
    EXPECT_EQ(v.toString(), "1010");  // умолчание — двоичное
    EXPECT_EQ(v.toString(Radix::BIN), "1010");
    EXPECT_EQ(v.toString(Radix::HEX), "a");
    EXPECT_EQ(v.toString(Radix::DEC), "10");

    LogicVector w(8);
    w.assignFromChars("00000001");
    EXPECT_EQ(w.toString(Radix::HEX), "01");  // два разряда: ведущий ноль сохранён
    EXPECT_EQ(w.toString(Radix::DEC), "1");   // у десятичного ширины разряда нет

    // Ширина не кратна основанию: старшая группа неполная.
    LogicVector odd(5);
    odd.assignFromChars("10011");
    EXPECT_EQ(odd.toString(Radix::HEX), "13");  // [4]=1, [3:0]=0011

    LogicVector six(6);
    six.assignFromChars("101011");
    EXPECT_EQ(six.toString(Radix::OCT), "53");  // 0b101011 == 0o53
}

// Правило IEEE 1800 для разряда с неопределёнными битами: группа целиком из
// одного неопределённого значения печатается им, смешанная — всегда 'x'.
TEST(Logic, RadixUnknownDigits) {
    LogicVector v(8);
    v.assignFromChars("0011xxxx");
    EXPECT_EQ(v.toString(Radix::HEX), "3x");  // группа целиком x

    v.assignFromChars("zzzz0001");
    EXPECT_EQ(v.toString(Radix::HEX), "z1");  // группа целиком z

    v.assignFromChars("1x1z0000");
    EXPECT_EQ(v.toString(Radix::HEX), "x0");  // смешанная группа -> x

    LogicVector m(4);
    m.assignFromChars("10x1");
    EXPECT_EQ(m.toString(Radix::HEX), "x");  // определённые биты не спасают
    m.assignFromChars("0zz1");
    EXPECT_EQ(m.toString(Radix::HEX), "x");  // 0/1 вперемешку с z — тоже x
    m.assignFromChars("xxxx");
    EXPECT_EQ(m.toString(Radix::HEX), "x");
    m.assignFromChars("zzzz");
    EXPECT_EQ(m.toString(Radix::HEX), "z");

    LogicVector o(6);
    o.assignFromChars("111zzz");
    EXPECT_EQ(o.toString(Radix::OCT), "7z");

    // Двоичное представление неопределённость не сворачивает.
    EXPECT_EQ(o.toString(Radix::BIN), "111zzz");
}

// Десятичное: знаковость приходит параметром (в битах её нет — она в
// VectorType::isSigned), любой x/z делает число непредставимым.
TEST(Logic, RadixDecimal) {
    LogicVector v(8);
    v.assignFromUint64(0xA5);
    EXPECT_EQ(v.toString(Radix::DEC), "165");
    EXPECT_EQ(v.toString(Radix::DEC, /*isSigned*/ true), "-91");  // 0xA5 как int8

    LogicVector pos(8);
    pos.assignFromUint64(0x7F);
    EXPECT_EQ(pos.toString(Radix::DEC, true), "127");  // знаковый бит не выставлен

    LogicVector zero(8);
    EXPECT_EQ(zero.toString(Radix::DEC), "0");
    EXPECT_EQ(zero.toString(Radix::DEC, true), "0");

    LogicVector one(1);
    one.assignFromChars("1");
    EXPECT_EQ(one.toString(Radix::DEC), "1");
    EXPECT_EQ(one.toString(Radix::DEC, true), "-1");  // ширина 1: единственный бит знаковый

    // Локализовать неопределённость в десятичном нельзя — непредставимо целиком.
    v.set(3, Logic::X);
    EXPECT_EQ(v.toString(Radix::DEC), "x");
    EXPECT_EQ(v.toString(Radix::DEC, true), "x");
    v.assignFromUint64(0xA5);
    v.set(0, Logic::Z);
    EXPECT_EQ(v.toString(Radix::DEC), "x");
}

// Десятичное шире 64 бит: арифметика произвольной точности, а не toUint64.
TEST(Logic, RadixDecimalWiderThan64) {
    LogicVector v(128);
    v.set(100);  // ровно 2^100
    EXPECT_EQ(v.toString(Radix::DEC), "1267650600228229401496703205376");
    EXPECT_EQ(v.toString(Radix::HEX), "00000010000000000000000000000000");
    EXPECT_FALSE(v.toUint64().has_value());  // в 64 бита не влезает

    LogicVector all(128);
    all.set();  // все единицы
    EXPECT_EQ(all.toString(Radix::DEC), "340282366920938463463374607431768211455");
    EXPECT_EQ(all.toString(Radix::DEC, /*isSigned*/ true), "-1");

    // Граница слова: 65 бит, старший выставлен.
    LogicVector w(65);
    w.set(64);
    EXPECT_EQ(w.toString(Radix::DEC), "18446744073709551616");  // 2^64
}

// Нулевая ширина: значения нет ни в одном основании — пустая строка, как у
// двоичного toString().
TEST(Logic, RadixZeroWidth) {
    const LogicVector v;
    ASSERT_TRUE(v.empty());
    EXPECT_EQ(v.toString(Radix::BIN), "");
    EXPECT_EQ(v.toString(Radix::OCT), "");
    EXPECT_EQ(v.toString(Radix::HEX), "");
    EXPECT_EQ(v.toString(Radix::DEC), "");
}

// Невладеющая вьюха форматирует так же — реализация живёт в ней.
TEST(Logic, RadixOnViewWithoutBvalPlane) {
    const std::uint64_t aval = 0xBEEF;
    const LogicVectorView v{&aval, nullptr, 16};
    EXPECT_EQ(v.toString(Radix::HEX), "beef");
    EXPECT_EQ(v.toString(Radix::DEC), "48879");
    EXPECT_EQ(v.toString(Radix::DEC, /*isSigned*/ true), "-16657");
}

// Основание вне Radix — ошибка вызывающей стороны, а не повод для UB или
// молчаливой пустой строки: приведённый мусор ловится броском.
TEST(Logic, RadixUnknownThrows) {
    LogicVector v(4);
    v.assignFromChars("1010");

    EXPECT_THROW((void)v.toString(static_cast<Radix>(99)), Exception);
    EXPECT_NO_THROW((void)v.toString(Radix::HEX));

    // Нулевая ширина отсекается раньше switch — там броска нет.
    const LogicVector empty;
    EXPECT_NO_THROW((void)empty.toString(static_cast<Radix>(99)));
}
