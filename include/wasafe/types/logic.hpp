#pragma once

#include <cstdint>

namespace WaSafe {

/// Четырёхзначная логика IEEE 1364/1800.
enum class Logic : std::uint8_t {
    ZERO = 0,  ///< '0'
    ONE = 1,   ///< '1'
    Z = 2,     ///< 'z' — высокоимпедансное состояние
    X = 3,     ///< 'x' — неизвестно/конфликт
};

[[nodiscard]] constexpr char toChar(Logic v) noexcept {
    switch (v) {
        case Logic::ZERO:
            return '0';
        case Logic::ONE:
            return '1';
        case Logic::Z:
            return 'z';
        case Logic::X:
            return 'x';
    }
    return 'x';
}

[[nodiscard]] constexpr Logic logicFromChar(char c) noexcept {
    switch (c) {
        case '0':
            return Logic::ZERO;
        case '1':
            return Logic::ONE;
        case 'z':
        case 'Z':
            return Logic::Z;
        default:
            return Logic::X;  // 'x','X' и всё прочее
    }
}

/// true, если значение принадлежит {0,1} (определено).
[[nodiscard]] constexpr bool is01(Logic v) noexcept {
    return v == Logic::ZERO || v == Logic::ONE;
}

/// Логическое НЕ (IEEE 1164): 0↔1; для неопределённых (X/Z) результат — X.
[[nodiscard]] constexpr Logic logicNot(Logic v) noexcept {
    switch (v) {
        case Logic::ZERO:
            return Logic::ONE;
        case Logic::ONE:
            return Logic::ZERO;
        default:
            return Logic::X;
    }
}

// --- Кодировка бит-планов (как s_vpi_vecval) ---------------------------------
// Каждый бит кодируется парой (a, b):
//   0 -> (0,0)   1 -> (1,0)   Z -> (0,1)   X -> (1,1)
// Это позволяет хранить вектор как два массива 64-битных слов: aval[] и bval[].
[[nodiscard]] constexpr Logic logicFromAb(unsigned a, unsigned b) noexcept {
    return static_cast<Logic>((a & 1u) | ((b & 1u) << 1));
}

[[nodiscard]] constexpr unsigned logicA(Logic v) noexcept {
    return static_cast<unsigned>(v) & 1u;
}

[[nodiscard]] constexpr unsigned logicB(Logic v) noexcept {
    return (static_cast<unsigned>(v) >> 1) & 1u;
}

}  // namespace WaSafe
