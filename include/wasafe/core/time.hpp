#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "wasafe/export.hpp"

namespace WaSafe {

/// Момент времени, выраженный в единицах временной сетки дампа (см. TimeScale).
/// 64-бит знаковый: ~9.2e18 единиц — достаточно даже для фемтосекундной сетки.
using TimeStamp = std::int64_t;

/// Маркер «время не задано».
inline constexpr TimeStamp kNoTime = std::numeric_limits<TimeStamp>::min();

/// Десятичная приставка единицы времени (показатель степени десяти, в секундах).
enum class TimeUnit : std::int8_t {
    FS = -15,
    PS = -12,
    NS = -9,
    US = -6,
    MS = -3,
    S = 0,
};

/// Масштаб времени дампа: одна единица TimeStamp = `scale * 10^exponent` секунд.
/// Например, "10 ns" => {exponent = -9, scale = 10}.
struct TimeScale {
    std::int32_t exponent = static_cast<std::int32_t>(TimeUnit::PS);  ///< показатель десяти
    std::int64_t scale = 1;                                           ///< множитель: 1, 10 или 100

    friend bool operator==(const TimeScale&, const TimeScale&) = default;
};

/// Полуоткрытый временной интервал [begin, end). Пустой при begin >= end.
struct TimeRange {
    TimeStamp begin = 0;
    TimeStamp end = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return begin >= end; }
    [[nodiscard]] constexpr TimeStamp size() const noexcept { return empty() ? 0 : end - begin; }
    [[nodiscard]] constexpr bool contains(TimeStamp t) const noexcept { return t >= begin && t < end; }

    [[nodiscard]] constexpr bool overlaps(const TimeRange& o) const noexcept { return begin < o.end && o.begin < end; }
    [[nodiscard]] constexpr TimeRange intersect(const TimeRange& o) const noexcept {
        return TimeRange{begin > o.begin ? begin : o.begin, end < o.end ? end : o.end};
    }
    /// Объединяющий интервал (минимальный, содержащий оба).
    [[nodiscard]] constexpr TimeRange hull(const TimeRange& o) const noexcept {
        return TimeRange{begin < o.begin ? begin : o.begin, end > o.end ? end : o.end};
    }

    friend auto operator<=>(const TimeRange&, const TimeRange&) = default;
};

/// Полный диапазон времени (используется как «загрузить всё»).
inline constexpr TimeRange kWholeTime{std::numeric_limits<TimeStamp>::min(), std::numeric_limits<TimeStamp>::max()};

/// Человекочитаемое представление масштаба ("1 ps", "10 ns").
[[nodiscard]] WASAFE_API std::string formatTimeScale(TimeScale ts);

/// Разбор строки масштаба ("10ns", "1 ps"); std::nullopt при ошибке.
[[nodiscard]] WASAFE_API std::optional<TimeScale> parseTimeScale(std::string_view text);

/// Перевод метки времени в секунды (double) согласно масштабу — для отображения.
[[nodiscard]] WASAFE_API double toSeconds(TimeStamp t, TimeScale ts) noexcept;

}  // namespace WaSafe
