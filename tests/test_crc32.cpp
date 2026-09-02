#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/crc32.hpp"

using namespace WaSafe;

namespace {

std::vector<std::byte> bytesOf(std::string_view s) {
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (const char c : s)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

// Сумма пишется в файл, поэтому её значения — часть формата: проверяются
// общеизвестными векторами CRC-32/ISO-HDLC, а не самими собой.
TEST(Crc32, MatchesReferenceVectors) {
    EXPECT_EQ(Crc32::compute(std::span<const std::byte>{}), 0u);
    EXPECT_EQ(Crc32::compute(bytesOf("a")), 0xE8B7'BE43u);
    EXPECT_EQ(Crc32::compute(bytesOf("123456789")), 0xCBF4'3926u);
    EXPECT_EQ(Crc32::compute(bytesOf("The quick brown fox jumps over the lazy dog")), 0x414F'A339u);
}

// Счёт по кускам должен давать то же, что счёт целиком: иначе метаданные,
// записанные по частям, не сошлись бы с проверкой при открытии.
TEST(Crc32, PiecewiseEqualsWhole) {
    const auto whole = bytesOf("The quick brown fox jumps over the lazy dog");

    for (const std::size_t cut : {std::size_t{0}, std::size_t{1}, whole.size() / 2, whole.size()}) {
        std::uint32_t crc = Crc32::kInit;
        crc = Crc32::update(crc, std::span{whole}.first(cut));
        crc = Crc32::update(crc, std::span{whole}.subspan(cut));
        EXPECT_EQ(crc, Crc32::compute(whole)) << "cut = " << cut;
    }
}

// То, ради чего сумма и заводится: одиночный перевёрнутый бит виден.
TEST(Crc32, DetectsSingleBitFlip) {
    auto data = bytesOf("wasafe store metadata");
    const std::uint32_t original = Crc32::compute(data);

    for (std::size_t i = 0; i < data.size(); ++i) {
        const std::byte saved = data[i];
        data[i] = saved ^ std::byte{0x01};
        EXPECT_NE(Crc32::compute(data), original) << "byte = " << i;
        data[i] = saved;
    }
}
