#pragma once

#include <cstdint>
#include <span>
#include <variant>

#include "wasafe/types/value_view.hpp"

namespace WaSafe {

/// Невладеющий поколоночный (structure-of-arrays) срез значений ОДНОГО потока:
/// всё необходимое, чтобы материализовать i-е изменение в ValueView. Спаны
/// указывают внутрь источника (in-memory Stream или DecodedBlock) и валидны,
/// пока тот жив. Декодер (valueAt) общий для всех storage-backend'ов, чтобы
/// поколоночная раскладка читалась в одном месте.
///
/// Раскладка зависит от вида значения, поэтому колонки хранятся как std::variant
/// с отдельной альтернативой (и конструктором) на каждый вид; default — пустой
/// (None) срез.
class WASAFE_API ColumnView final {
public:
    ColumnView() noexcept = default;
    /// Logic: планы хранятся ОТДЕЛЬНО, i-е изменение занимает [i*words, i*words+words)
    /// в каждом из них. Пустой bval означает двухзначный поток (все b-биты нулевые) —
    /// тогда источник его не хранит, а ValueView отдаётся с bval == nullptr.
    ColumnView(std::uint32_t width, std::span<const std::uint64_t> aval, std::span<const std::uint64_t> bval) noexcept :
            columns_{Logic{width, aval, bval}} {}
    /// Real: одно double на изменение.
    explicit ColumnView(std::span<const double> reals) noexcept : columns_{Real{reals}} {}
    /// String: арена символов и смещения (count + 1) в неё.
    ColumnView(std::span<const char> arena, std::span<const std::uint32_t> offsets) noexcept :
            columns_{String{arena, offsets}} {}

    /// Невладеющее значение i-го изменения (указывает ВНУТРЬ источника).
    /// Вне границ соответствующей колонки возвращает пустой ValueView.
    [[nodiscard]] ValueView valueAt(std::size_t i) const noexcept;

private:
    struct Logic {
        std::uint32_t width = 0;
        std::span<const std::uint64_t> aval;
        std::span<const std::uint64_t> bval;  ///< пуст => поток двухзначный
    };
    struct Real {
        std::span<const double> values;
    };
    struct String {
        std::span<const char> arena;
        std::span<const std::uint32_t> offsets;
    };

private:
    std::variant<std::monostate, Logic, Real, String> columns_;
};

}  // namespace WaSafe
