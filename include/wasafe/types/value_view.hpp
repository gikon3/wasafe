#pragma once

#include <string_view>
#include <variant>

#include "wasafe/types/logic_vector.hpp"
#include "wasafe/types/logic_vector_view.hpp"

namespace WaSafe {

/// Категория хранимого значения.
enum class ValueKind : std::uint8_t {
    NONE,       ///< отсутствует
    LOGIC,      ///< вектор 4-значной логики (скаляр — частный случай ширины 1)
    REAL,       ///< вещественное (real)
    STRING,     ///< строка
    AGGREGATE,  ///< собранный композит (struct/array) — список вложенных Value
};

/// Невладеющее значение «как оно лежит в storage». Возвращается из горячих путей
/// (курсоры, value_at) и валидно до следующего обращения к тому же курсору/storage.
/// Для листьев это Logic/Real/String; агрегаты собираются отдельно в Value.
class WASAFE_API ValueView final {
public:
    ValueView() noexcept = default;
    ValueView(LogicVectorView logic) noexcept : value_{logic} {}
    ValueView(double real) noexcept : value_{real} {}
    ValueView(std::string_view string) noexcept : value_{string} {}
    ValueView(const LogicVector& value) noexcept : value_{value} {}

    [[nodiscard]] bool valid() const noexcept { return !std::holds_alternative<std::monostate>(value_); }

    [[nodiscard]] ValueKind kind() const noexcept {
        switch (value_.index()) {
            case 1:
                return ValueKind::LOGIC;
            case 2:
                return ValueKind::REAL;
            case 3:
                return ValueKind::STRING;
            default:
                return ValueKind::NONE;
        }
    }

    [[nodiscard]] LogicVectorView logic() const { return std::get<LogicVectorView>(value_); }
    [[nodiscard]] double real() const { return std::get<double>(value_); }
    [[nodiscard]] std::string_view string() const { return std::get<std::string_view>(value_); }

private:
    std::variant<std::monostate, LogicVectorView, double, std::string_view> value_;
};

}  // namespace WaSafe
