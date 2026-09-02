#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

/// CRC-32 (полином IEEE 802.3 в отражённом виде, 0xEDB88320) — тот же, что у
/// zip, png и zlib. Считается табличным byte-wise способом: таблица на 256
/// записей заменяет восемь битовых шагов одной загрузкой.
namespace WaSafe::Crc32 {

/// Таблица собирается в compile time: в бинарнике лежит готовый килобайт.
inline constexpr std::array<std::uint32_t, 256> kTable = [] {
    constexpr std::uint32_t kPolynomial = 0xEDB8'8320u;
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit)
            c = ((c & 1u) != 0u) ? (kPolynomial ^ (c >> 1u)) : (c >> 1u);
        table[i] = c;
    }
    return table;
}();

/// Продолжить счёт с ранее полученного значения — для данных, приходящих
/// кусками. Начальное значение для первого куска — kInit.
inline constexpr std::uint32_t kInit = 0u;

[[nodiscard]] inline std::uint32_t update(std::uint32_t crc, std::span<const std::byte> data) noexcept {
    std::uint32_t c = ~crc;
    for (const std::byte b : data)
        c = kTable[(c ^ static_cast<std::uint32_t>(b)) & 0xFFu] ^ (c >> 8u);
    return ~c;
}

[[nodiscard]] inline std::uint32_t compute(std::span<const std::byte> data) noexcept {
    return update(kInit, data);
}

}  // namespace WaSafe::Crc32
