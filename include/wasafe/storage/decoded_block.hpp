#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "wasafe/core/exception.hpp"
#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"
#include "wasafe/types/column_view.hpp"
#include "wasafe/types/time_column.hpp"
#include "wasafe/types/value.hpp"

namespace WaSafe {

/// Декодированный блок изменений ОДНОГО потока.
///
/// Поколоночное (structure-of-arrays) представление: массив меток времени
/// отделён от значений, что даёт быстрый бинарный поиск по времени без
/// разбора значений. Это распакованная форма блока; на диске блок лежит как
/// плоский байтовый буфер (см. encode_block/decode_block), при необходимости
/// дополнительно сжатый кодеком из BlockRef.
///
/// Соглашение о значениях: метки `times` строго возрастают и содержат только
/// РЕАЛЬНЫЕ изменения (без синтетических «переносов»). Значение в произвольный
/// момент t восстанавливается как последнее изменение с временем <= t; если t
/// меньше первого изменения блока, оно берётся из предыдущего блока — поэтому
/// каждый блок обязан содержать хотя бы одно изменение.
/// Внутреннее представление инкапсулировано: наполнение — через конструктор и
/// append(), нарезка подблоков — через slice(), чтение — через аксессоры и
/// невладеющий columns(). Вид блока задаётся активной альтернативой values_ —
/// отдельного поля kind не требуется. Доступ к «сырому» поколоночному хранилищу
/// есть только у сериализации (encodeBlock/decodeBlock — friend).
class WASAFE_API DecodedBlock final {
public:
    DecodedBlock() = default;
    /// Создать пустой блок заданного вида и битовой ширины (width == 0 для real/string).
    DecodedBlock(ValueKind kind, std::uint32_t width);

    /// Дописать одно изменение значения в момент t (время монотонно возрастает).
    void append(TimeStamp t, ValueView v);

    /// Вырезать подблок изменений [lo, hi) как самостоятельный DecodedBlock
    /// (смещения строковой арены ребазируются от нуля).
    [[nodiscard]] DecodedBlock slice(std::size_t lo, std::size_t hi) const;

    [[nodiscard]] std::size_t count() const noexcept { return times_.size(); }
    [[nodiscard]] bool empty() const noexcept { return times_.empty(); }
    [[nodiscard]] const TimeColumn& times() const noexcept { return times_; }

    /// Невладеющий поколоночный срез значений блока (декодер — в ColumnView).
    [[nodiscard]] ColumnView columns() const noexcept;

    /// Невладеющее значение i-го изменения (указывает ВНУТРЬ этого блока —
    /// валидно, пока жив блок).
    [[nodiscard]] ValueView valueAtIndex(std::size_t i) const noexcept;

    /// Приблизительный объём в байтах (для учёта в LRU-кэше).
    [[nodiscard]] std::size_t byteSize() const noexcept;

private:
    // Альтернативы хранилища значений; активная определяет вид блока.
    /// Logic: бит-планы лежат ОТДЕЛЬНО, у изменения i — слова
    /// [i*words, i*words+words) в каждом, где words = wordsFor(width).
    /// bval пуст, пока в блоке не встретилось x/z (двухзначный блок): это
    /// вдвое сокращает память. Первое же четырёхзначное значение «повышает»
    /// блок — bval разворачивается нулями под уже записанные изменения.
    struct LogicStore {
        std::uint32_t width = 0;
        std::vector<std::uint64_t> aval;
        std::vector<std::uint64_t> bval;
    };
    struct RealStore {
        std::vector<double> values;  ///< размер = count
    };
    struct StringStore {
        std::vector<char> arena;             ///< арена символов
        std::vector<std::uint32_t> offsets;  ///< размер count+1, смещения в арену
    };

private:
    friend WASAFE_API std::vector<std::byte> encodeBlock(const DecodedBlock& block);
    friend WASAFE_API DecodedBlock decodeBlock(std::span<const std::byte> raw);
    /// Вид блока, выведенный из активной альтернативы (для сериализации).
    [[nodiscard]] ValueKind kind() const noexcept;
    /// Битовая ширина (для logic; иначе 0).
    [[nodiscard]] std::uint32_t width() const noexcept;

private:
    TimeColumn times_;  ///< размер = count, строго возрастает
    std::variant<std::monostate, LogicStore, RealStore, StringStore> values_;
};

/// Сериализовать полезную нагрузку блока в плоский буфер (без сжатия).
/// Формат версии 1, порядок байт — хостовый (TODO: переносимый little-endian).
[[nodiscard]] WASAFE_API std::vector<std::byte> encodeBlock(const DecodedBlock& block);

/// Разобрать буфер, полученный от BlockSource (уже распакованный кодеком).
[[nodiscard]] WASAFE_API DecodedBlock decodeBlock(std::span<const std::byte> raw);

}  // namespace WaSafe
