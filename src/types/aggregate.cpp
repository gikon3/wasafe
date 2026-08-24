#include "wasafe/types/aggregate.hpp"

#include "wasafe/types/value.hpp"

namespace WaSafe {

Aggregate::Aggregate(std::vector<Value> elements) noexcept : elements_{std::move(elements)} {
}

bool Aggregate::empty() const noexcept {
    return elements_.empty();
}

std::size_t Aggregate::size() const noexcept {
    return elements_.size();
}

Aggregate::ConstIterator Aggregate::begin() const noexcept {
    return elements_.begin();
}

Aggregate::ConstIterator Aggregate::end() const noexcept {
    return elements_.end();
}

Aggregate::Iterator Aggregate::begin() noexcept {
    return elements_.begin();
}

Aggregate::Iterator Aggregate::end() noexcept {
    return elements_.end();
}

const Value& Aggregate::operator[](std::size_t i) const noexcept {
    return elements_[i];
}

Value& Aggregate::operator[](std::size_t i) noexcept {
    return elements_[i];
}

}  // namespace WaSafe
