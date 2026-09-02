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

/// Ссылочный счёт «в лоб», без таблиц: восемь битовых шагов на байт, прямо по
/// определению полинома. Нужен, чтобы проверять табличную реализацию не только
/// известными векторами: при slicing-by-8 таблиц восемь, и ошибка в одной из них
/// на коротких данных эталонными векторами может не задеться.
std::uint32_t crcByDefinition(std::span<const std::byte> data) {
    constexpr std::uint32_t kPolynomial = 0xEDB8'8320u;
    std::uint32_t c = ~0u;
    for (const std::byte b : data) {
        c ^= static_cast<std::uint32_t>(b);
        for (int bit = 0; bit < 8; ++bit)
            c = ((c & 1u) != 0u) ? (kPolynomial ^ (c >> 1u)) : (c >> 1u);
    }
    return ~c;
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

// Табличный счёт против определения полинома — на каждой длине от нуля до
// восьмидесяти, то есть на всех сочетаниях «сколько восьмёрок и сколько байт
// хвоста». Эталонные векторы выше проверяют результат, а это — таблицы.
TEST(Crc32, MatchesDefinitionOnEveryLength) {
    std::vector<std::byte> data;
    data.reserve(80);
    std::uint32_t next = 0x1234'5678u;
    for (std::size_t i = 0; i < 80; ++i) {
        next = next * 1'103'515'245u + 12'345u;  // воспроизводимый мусор
        data.push_back(static_cast<std::byte>((next >> 16u) & 0xFFu));
    }

    for (std::size_t n = 0; n <= data.size(); ++n) {
        const std::span<const std::byte> part{data.data(), n};
        EXPECT_EQ(Crc32::compute(part), crcByDefinition(part)) << "длина = " << n;
    }
}
