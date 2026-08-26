#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/byte_io.hpp"
#include "wasafe/storage/decoded_block.hpp"

using namespace WaSafe;

namespace {

/// Удобный литерал для эталонных последовательностей.
std::vector<std::byte> bytesOf(std::initializer_list<int> vals) {
    std::vector<std::byte> out;
    out.reserve(vals.size());
    for (const int v : vals)
        out.push_back(static_cast<std::byte>(v));
    return out;
}

}  // namespace

// Скаляры ложатся младшим байтом вперёд — независимо от машины.
TEST(ByteIo, ScalarsAreLittleEndian) {
    std::vector<std::byte> buf;
    ByteWriter w{buf};
    w.u32(0x1122'3344u);
    EXPECT_EQ(buf, bytesOf({0x44, 0x33, 0x22, 0x11}));

    buf.clear();
    w.u64(0x0102'0304'0506'0708ull);
    EXPECT_EQ(buf, bytesOf({0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}));

    buf.clear();
    w.i32(-2);
    EXPECT_EQ(buf, bytesOf({0xFE, 0xFF, 0xFF, 0xFF}));
}

// Массивы — та же раскладка, что у одиночных значений подряд.
TEST(ByteIo, ArraysAreLittleEndian) {
    std::vector<std::byte> buf;
    ByteWriter w{buf};
    const std::vector<std::uint32_t> src{1u, 0x0A0B'0C0Du};
    w.array(std::span{src});
    EXPECT_EQ(buf, bytesOf({0x01, 0x00, 0x00, 0x00, 0x0D, 0x0C, 0x0B, 0x0A}));
}

// Round-trip всех поддержанных типов.
TEST(ByteIo, RoundTrip) {
    std::vector<std::byte> buf;
    ByteWriter w{buf};
    w.u8(0xAB);
    w.u32(0xDEAD'BEEFu);
    w.u64(0xFEED'FACE'CAFE'BABEull);
    w.i32(-123456);
    w.i64(-9'000'000'000ll);
    w.f64(-0.125);
    w.varint(300);
    w.svarint(-7);
    w.str("сигнал[3]");

    const std::vector<std::uint64_t> words{1ull, ~0ull, 42ull};
    const std::vector<double> reals{0.5, -2.25};
    w.array(std::span{words});
    w.array(std::span{reals});

    ByteReader r{buf};
    std::uint8_t u8v = 0;
    std::uint32_t u32v = 0;
    std::uint64_t u64v = 0;
    std::int32_t i32v = 0;
    std::int64_t i64v = 0;
    double f64v = 0;
    std::uint64_t varv = 0;
    std::int64_t svarv = 0;
    std::string strv;

    ASSERT_TRUE(r.u8(u8v));
    ASSERT_TRUE(r.u32(u32v));
    ASSERT_TRUE(r.u64(u64v));
    ASSERT_TRUE(r.i32(i32v));
    ASSERT_TRUE(r.i64(i64v));
    ASSERT_TRUE(r.f64(f64v));
    ASSERT_TRUE(r.varint(varv));
    ASSERT_TRUE(r.svarint(svarv));
    ASSERT_TRUE(r.str(strv));

    EXPECT_EQ(u8v, 0xAB);
    EXPECT_EQ(u32v, 0xDEAD'BEEFu);
    EXPECT_EQ(u64v, 0xFEED'FACE'CAFE'BABEull);
    EXPECT_EQ(i32v, -123456);
    EXPECT_EQ(i64v, -9'000'000'000ll);
    EXPECT_EQ(f64v, -0.125);
    EXPECT_EQ(varv, 300u);
    EXPECT_EQ(svarv, -7);
    EXPECT_EQ(strv, "сигнал[3]");

    std::vector<std::uint64_t> gotWords(words.size());
    std::vector<double> gotReals(reals.size());
    ASSERT_TRUE(r.array(std::span{gotWords}));
    ASSERT_TRUE(r.array(std::span{gotReals}));
    EXPECT_EQ(gotWords, words);
    EXPECT_EQ(gotReals, reals);
    EXPECT_EQ(r.remaining(), 0u);
}

// Зигзаг: малые по модулю отрицательные стоят один байт, а не десять.
TEST(ByteIo, ZigzagIsCompact) {
    std::vector<std::byte> buf;
    ByteWriter w{buf};
    w.svarint(-1);
    EXPECT_EQ(buf.size(), 1u);

    buf.clear();
    w.varint(0x7F);
    EXPECT_EQ(buf.size(), 1u);
    buf.clear();
    w.varint(0x80);
    EXPECT_EQ(buf.size(), 2u);
}

// Чтение за границей буфера возвращает false, а не бросает и не портит память.
TEST(ByteIo, ReadPastEndFails) {
    std::vector<std::byte> buf;
    ByteWriter w{buf};
    w.u32(1);

    ByteReader r{buf};
    std::uint64_t u64v = 0;
    EXPECT_FALSE(r.u64(u64v));  // в буфере всего 4 байта

    std::uint32_t u32v = 0;
    ASSERT_TRUE(r.u32(u32v));
    EXPECT_FALSE(r.u32(u32v));  // буфер исчерпан

    std::vector<std::uint64_t> words(2);
    ByteReader r2{buf};
    EXPECT_FALSE(r2.array(std::span{words}));
}

// Эталонные байты блока: раскладка формата зафиксирована побайтово, поэтому
// регресс по порядку байт виден без BE-машины.
TEST(ByteIo, BlockGoldenBytes) {
    DecodedBlock b{ValueKind::LOGIC, 4};
    LogicVector v(4);
    v.assignFromChars("0001");
    b.append(0, ValueView{v});
    v.assignFromChars("0010");
    b.append(10, ValueView{v});

    const auto kind = static_cast<int>(ValueKind::LOGIC);
    // clang-format off
    const auto expected = bytesOf({
            0x57, 0x44, 0x42, 0x33,  // magic 'WDB3'
            kind, 0x00,              // kind, flags (двухзначный блок — без bval)
            0x04, 0x00, 0x00, 0x00,  // width = 4
            0x02, 0x00, 0x00, 0x00,  // count = 2
            0x00,                    // время[0] = 0, зигзаг
            0x0A,                    // дельта = 10
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // aval[0] = 0b0001
            0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // aval[1] = 0b0010
    });
    // clang-format on

    EXPECT_EQ(encodeBlock(b), expected);
}
