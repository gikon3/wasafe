#pragma once

#include <cstdint>
#include <vector>

#include "wasafe/export.hpp"

namespace WaSafe {

class Value;

/// Композит (struct/array): упорядоченный список вложенных Value.
class WASAFE_API Aggregate final {
public:
    using Iterator = std::vector<Value>::iterator;
    using ConstIterator = std::vector<Value>::const_iterator;

public:
    Aggregate() noexcept = default;
    explicit Aggregate(std::vector<Value> elements) noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] ConstIterator begin() const noexcept;
    [[nodiscard]] ConstIterator end() const noexcept;
    [[nodiscard]] Iterator begin() noexcept;
    [[nodiscard]] Iterator end() noexcept;

    [[nodiscard]] const Value& operator[](std::size_t i) const noexcept;
    [[nodiscard]] Value& operator[](std::size_t i) noexcept;

private:
    std::vector<Value> elements_;
};

}  // namespace WaSafe
