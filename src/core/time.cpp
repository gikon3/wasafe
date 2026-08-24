#include "wasafe/core/time.hpp"

#include <array>
#include <cmath>

namespace WaSafe {

namespace {
struct UnitText {
    std::int32_t exp;
    std::string_view text;
};
constexpr std::array kUnits{
        UnitText{0, "s"},
        UnitText{-3, "ms"},
        UnitText{-6, "us"},
        UnitText{-9, "ns"},
        UnitText{-12, "ps"},
        UnitText{-15, "fs"},
};
}  // namespace

std::string formatTimeScale(TimeScale ts) {
    for (const auto& u : kUnits) {
        if (u.exp == ts.exponent) {
            return std::to_string(ts.scale) + " " + std::string(u.text);
        }
    }
    // TODO(impl): нормализовать произвольный exponent к ближайшей приставке.
    return std::to_string(ts.scale) + "e" + std::to_string(ts.exponent) + " s";
}

std::optional<TimeScale> parseTimeScale(std::string_view text) {
    // TODO(impl): полноценный разбор "10 ns" / "1ps" с пробелами и регистром.
    (void)text;
    return std::nullopt;
}

double toSeconds(TimeStamp t, TimeScale ts) noexcept {
    return static_cast<double>(t) * static_cast<double>(ts.scale) * std::pow(10.0, static_cast<double>(ts.exponent));
}

}  // namespace WaSafe
