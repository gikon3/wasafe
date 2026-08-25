#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"

namespace WaSafe {

/// Колонка меток времени одного потока — владеющая, с уплотнённым хранением.
///
/// Метки внутри одного блока обычно плотные, поэтому вместо восьми байт на
/// изменение хранится база блока и 32-битные смещения от неё. Если разность
/// перестаёт помещаться в 32 бита (медленный сигнал, длинная симуляция или
/// вся история потока целиком в MemoryStorage), колонка ОДНОКРАТНО «повышается»
/// до абсолютных 64-битных меток и дальше работает как обычный массив.
///
/// Раскладка спрятана за индексным интерфейсом (в духе ColumnView для значений):
/// наружу отдаются только позиции, а не итераторы, поэтому представление можно
/// менять, не трогая читающую логику. Все операции — O(1) либо O(log n).
class WASAFE_API TimeColumn final {
public:
    /// Дописать метку. Время не должно убывать (инвариант потока изменений).
    void append(TimeStamp t);

    [[nodiscard]] std::size_t size() const noexcept { return wide_.empty() ? narrow_.size() : wide_.size(); }
    [[nodiscard]] bool empty() const noexcept { return narrow_.empty() && wide_.empty(); }

    /// Метка по позиции. Вне границ — kNoTime.
    [[nodiscard]] TimeStamp operator[](std::size_t i) const noexcept;
    [[nodiscard]] TimeStamp front() const noexcept { return (*this)[0]; }
    [[nodiscard]] TimeStamp back() const noexcept { return empty() ? kNoTime : (*this)[size() - 1]; }

    /// Первая позиция со временем > t (аналог upper_bound); size(), если такой нет.
    [[nodiscard]] std::size_t upperBound(TimeStamp t) const noexcept;
    /// Первая позиция со временем >= t (аналог lower_bound); size(), если такой нет.
    [[nodiscard]] std::size_t lowerBound(TimeStamp t) const noexcept;

    /// Подколонка [lo, hi). Представление выбирается заново, поэтому срез
    /// повышенной колонки может снова оказаться узким.
    [[nodiscard]] TimeColumn slice(std::size_t lo, std::size_t hi) const;

    /// Объём полезных данных в байтах (для учёта в бюджетах и LRU-кэше).
    [[nodiscard]] std::size_t byteSize() const noexcept {
        return narrow_.size() * sizeof(std::uint32_t) + wide_.size() * sizeof(TimeStamp);
    }

    void clear() noexcept {
        narrow_.clear();
        wide_.clear();
        base_ = 0;
    }

private:
    /// Развернуть узкое представление в абсолютные метки.
    void promote();

    /// Разность t - base_ в беззнаковой арифметике. Вызывать только при t >= base_:
    /// прямое вычитание в int64 переполнилось бы на краях диапазона (kWholeTime
    /// приносит сюда min/max TimeStamp).
    [[nodiscard]] std::uint64_t offsetFromBase(TimeStamp t) const noexcept {
        return static_cast<std::uint64_t>(t) - static_cast<std::uint64_t>(base_);
    }

private:
    static constexpr std::uint64_t kMaxNarrow = 0xFFFF'FFFFull;

private:
    TimeStamp base_ = 0;                 ///< метка нулевой позиции узкого представления
    std::vector<std::uint32_t> narrow_;  ///< смещения от base_, пока влезают в 32 бита
    std::vector<TimeStamp> wide_;        ///< абсолютные метки после повышения
};

}  // namespace WaSafe
