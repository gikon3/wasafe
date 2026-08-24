#include "wasafe/types/value.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>

namespace WaSafe {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

ValueKind Value::kind() const noexcept {
    switch (storage_.index()) {
        case 1:
            return ValueKind::LOGIC;
        case 2:
            return ValueKind::REAL;
        case 3:
            return ValueKind::STRING;
        case 4:
            return ValueKind::AGGREGATE;
        default:
            return ValueKind::NONE;
    }
}

// NOLINTNEXTLINE(bugprone-exception-escape)
Value::operator ValueView() const noexcept {
    // clang-format off
    return std::visit(
            Overloaded{
                [](const LogicVector& v) { return ValueView{v}; },
                [](double v) { return ValueView{v}; },
                [](const std::string& v) { return ValueView{v}; },
                [](auto&&) { return ValueView{}; }
            },
            storage_);
    // clang-format on
}

}  // namespace WaSafe
