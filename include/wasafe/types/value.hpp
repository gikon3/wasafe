#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "wasafe/export.hpp"
#include "wasafe/types/aggregate.hpp"
#include "wasafe/types/logic_vector.hpp"
#include "wasafe/types/logic_vector_view.hpp"
#include "wasafe/types/value_view.hpp"

namespace WaSafe {

/// Владеющее значение (аналог boost::json::value): рекурсивный variant из листовых
/// видов и композита (Aggregate). Используется там, где нужна стабильная копия
/// (снимок) или сборка композита из листьев. Доступ по виду — как у boost::json:
/// is* (запрос), as* (ссылка, std::get бросает при несовпадении), if* (указатель
/// либо nullptr).
class WASAFE_API Value final {
public:
    Value() = default;
    explicit Value(LogicVector v) : storage_{std::move(v)} {}
    explicit Value(double v) : storage_{v} {}
    explicit Value(std::string v) : storage_{std::move(v)} {}
    explicit Value(Aggregate v) : storage_{std::move(v)} {}
    explicit Value(LogicVectorView v) : storage_{LogicVector{v}} {}

    [[nodiscard]] ValueKind kind() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return kind() != ValueKind::NONE; }

    [[nodiscard]] bool isLogic() const noexcept { return kind() == ValueKind::LOGIC; }
    [[nodiscard]] bool isReal() const noexcept { return kind() == ValueKind::REAL; }
    [[nodiscard]] bool isString() const noexcept { return kind() == ValueKind::STRING; }
    [[nodiscard]] bool isAggregate() const noexcept { return kind() == ValueKind::AGGREGATE; }

    [[nodiscard]] LogicVector& asLogic() { return std::get<LogicVector>(storage_); }
    [[nodiscard]] const LogicVector& asLogic() const { return std::get<LogicVector>(storage_); }
    [[nodiscard]] double& asReal() { return std::get<double>(storage_); }
    [[nodiscard]] double asReal() const { return std::get<double>(storage_); }
    [[nodiscard]] std::string& asString() { return std::get<std::string>(storage_); }
    [[nodiscard]] const std::string& asString() const { return std::get<std::string>(storage_); }
    [[nodiscard]] Aggregate& asAggregate() { return std::get<Aggregate>(storage_); }
    [[nodiscard]] const Aggregate& asAggregate() const { return std::get<Aggregate>(storage_); }

    [[nodiscard]] LogicVector* ifLogic() noexcept { return std::get_if<LogicVector>(&storage_); }
    [[nodiscard]] const LogicVector* ifLogic() const noexcept { return std::get_if<LogicVector>(&storage_); }
    [[nodiscard]] double* ifReal() noexcept { return std::get_if<double>(&storage_); }
    [[nodiscard]] const double* ifReal() const noexcept { return std::get_if<double>(&storage_); }
    [[nodiscard]] std::string* ifString() noexcept { return std::get_if<std::string>(&storage_); }
    [[nodiscard]] const std::string* ifString() const noexcept { return std::get_if<std::string>(&storage_); }
    [[nodiscard]] Aggregate* ifAggregate() noexcept { return std::get_if<Aggregate>(&storage_); }
    [[nodiscard]] const Aggregate* ifAggregate() const noexcept { return std::get_if<Aggregate>(&storage_); }

    [[nodiscard]] operator ValueView() const noexcept;

private:
    using Storage = std::variant<std::monostate, LogicVector, double, std::string, Aggregate>;

private:
    Storage storage_;
};

}  // namespace WaSafe
