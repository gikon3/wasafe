#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>

#include "wasafe/export.hpp"
#include "wasafe/types/logic.hpp"

namespace WaSafe {

/// Невладеющее представление вектора 4-значной логики.
/// Данные лежат в двух бит-планах (aval/bval), упакованных в 64-битные слова
/// (кодировка из logic.hpp). Указатели валидны, пока жива источниковая память.
///
/// ДВУХЗНАЧНАЯ ФОРМА: bval == nullptr означает, что все b-биты нулевые, то есть
/// значение состоит только из 0/1. Источник в этом случае вправе не хранить
/// bval-план вовсе (см. ColumnView) — это вдвое сокращает память на изменение,
/// а чтение становится дешевле на одно обращение. Аксессор bval() при этом
/// отдаёт ПУСТОЙ span (span ненулевой длины из нулевого указателя был бы UB),
/// поэтому потребители обязаны трактовать пустой bval как «все нули».
class WASAFE_API LogicVectorView final {
public:
    using WordType = std::uint64_t;

public:
    LogicVectorView() = default;
    /// @param bval  nullptr — значение двухзначное (все b-биты нулевые).
    LogicVectorView(const WordType* aval, const WordType* bval, std::uint32_t width) noexcept :
            a_{aval}, b_{bval}, width_{width} {}

    [[nodiscard]] bool empty() const noexcept { return width_ == 0; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }

    [[nodiscard]] std::span<const WordType> aval() const noexcept { return {a_, wordsFor(width_)}; }
    /// Пустой span, если значение двухзначное (bval не хранится).
    [[nodiscard]] std::span<const WordType> bval() const noexcept {
        return b_ == nullptr ? std::span<const WordType>{} : std::span<const WordType>{b_, wordsFor(width_)};
    }

    [[nodiscard]] bool isTwoState() const noexcept;
    /// Строка вида "10xz", старший бит — слева (MSB..LSB).
    [[nodiscard]] std::string toString() const;
    [[nodiscard]] std::optional<std::uint64_t> toUint64() const noexcept;

    /// Число 64-битных слов под вектор заданной ширины.
    [[nodiscard]] static constexpr std::uint32_t wordsFor(std::uint32_t width) noexcept {
        return (width + kWordWidth - 1) / kWordWidth;
    }

    [[nodiscard]] Logic operator[](std::uint32_t bit) const noexcept;

public:
    static constexpr std::uint32_t kWordWidth = std::numeric_limits<WordType>::digits;

private:
    const WordType* a_ = nullptr;
    const WordType* b_ = nullptr;
    std::uint32_t width_ = 0;
};

}  // namespace WaSafe
