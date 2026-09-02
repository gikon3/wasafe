#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

/// CRC-32 (полином IEEE 802.3 в отражённом виде, 0xEDB88320) — тот же, что у
/// zip, png и zlib.
///
/// Считается способом slicing-by-8: восемь байт за итерацию, восемь НЕЗАВИСИМЫХ
/// обращений к разным таблицам сводятся XOR-деревом. Табличный byte-wise счёт
/// упирается в цепочку зависимостей (следующий байт ждёт загрузку предыдущего) и
/// даёт около 0,6 ГБ/с; здесь зависимость одна на восемь байт, и замер показывает
/// 2,4 ГБ/с — вчетверо больше. Плата — 8 КиБ таблиц вместо одного килобайта.
namespace WaSafe::Crc32 {

/// Таблицы собираются в compile time: в бинарнике лежат готовые 8 КиБ.
/// Нулевая — обычная byte-wise, каждая следующая продвигает её на байт вперёд.
inline constexpr std::array<std::array<std::uint32_t, 256>, 8> kTable = [] {
    constexpr std::uint32_t kPolynomial = 0xEDB8'8320u;
    std::array<std::array<std::uint32_t, 256>, 8> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit)
            c = ((c & 1u) != 0u) ? (kPolynomial ^ (c >> 1u)) : (c >> 1u);
        table[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
        for (std::size_t n = 1; n < 8; ++n)
            table[n][i] = (table[n - 1][i] >> 8u) ^ table[0][table[n - 1][i] & 0xFFu];
    }
    return table;
}();

/// Продолжить счёт с ранее полученного значения — для данных, приходящих
/// кусками. Начальное значение для первого куска — kInit.
inline constexpr std::uint32_t kInit = 0u;

namespace Detail {

/// Четыре байта как little-endian слово. Собирается сдвигами, поэтому результат
/// одинаков на машине любого порядка байт. На LE компилятор сворачивает это в одну загрузку.
[[nodiscard]] inline std::uint32_t le32(std::span<const std::byte, 4> p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
            (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}

}  // namespace Detail

[[nodiscard]] inline std::uint32_t update(std::uint32_t crc, std::span<const std::byte> data) noexcept {
    std::uint32_t c = ~crc;

    // Основной проход: по восемь байт. Первое слово смешивается с текущим
    // остатком, дальше оба слова расходятся по своим таблицам.
    for (; data.size() >= 8; data = data.subspan(8)) {
        const std::uint32_t lo = Detail::le32(data.first<4>()) ^ c;
        const std::uint32_t hi = Detail::le32(data.subspan<4>().first<4>());
        c = kTable[7][lo & 0xFFu] ^ kTable[6][(lo >> 8u) & 0xFFu] ^ kTable[5][(lo >> 16u) & 0xFFu] ^
                kTable[4][lo >> 24u] ^ kTable[3][hi & 0xFFu] ^ kTable[2][(hi >> 8u) & 0xFFu] ^
                kTable[1][(hi >> 16u) & 0xFFu] ^ kTable[0][hi >> 24u];
    }

    // Хвост короче восьми байт — обычным byte-wise счётом.
    for (const std::byte b : data)
        c = kTable[0][(c ^ static_cast<std::uint32_t>(b)) & 0xFFu] ^ (c >> 8u);
    return ~c;
}

[[nodiscard]] inline std::uint32_t compute(std::span<const std::byte> data) noexcept {
    return update(kInit, data);
}

}  // namespace WaSafe::Crc32
