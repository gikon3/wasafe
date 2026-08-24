#include "wasafe/types/column_view.hpp"

namespace WaSafe {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
ValueView ColumnView::valueAt(std::size_t i) const noexcept {
    // clang-format off
    return std::visit(
            Overloaded{
                [](std::monostate) -> ValueView { return {}; },
                [i](const Logic& c) -> ValueView {
                    const std::uint32_t words = LogicVectorView::wordsFor(c.width);
                    const std::size_t base = i * words;
                    if (base + words > c.aval.size())
                        return {};
                    // Пустой bval => поток двухзначный: вьюха получает nullptr.
                    const std::uint64_t* b = c.bval.empty() ? nullptr : c.bval.data() + base;
                    return LogicVectorView{c.aval.data() + base, b, c.width};
                },
                [i](const Real& c) -> ValueView {
                    return i < c.values.size() ? ValueView{c.values[i]} : ValueView{};
                },
                [i](const String& c) -> ValueView {
                    if (i + 1 >= c.offsets.size())
                        return {};
                    const std::uint32_t a = c.offsets[i];
                    const std::uint32_t b = c.offsets[i + 1];
                    return std::string_view{c.arena.data() + a, b - a};
                },
            },
            columns_);
    // clang-format on
}

}  // namespace WaSafe
