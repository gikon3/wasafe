#include "wasafe/types/time_column.hpp"

#include <algorithm>
#include <functional>

namespace WaSafe {

void TimeColumn::append(TimeStamp t) {
    if (!wide_.empty()) {
        wide_.push_back(t);
        return;
    }
    if (narrow_.empty()) {
        base_ = t;
        narrow_.push_back(0);
        return;
    }
    // Убывающее время нарушило бы инвариант потока; на всякий случай не режем
    // его молча, а уходим в абсолютное представление, где оно хотя бы сохранится.
    if (t < base_ || offsetFromBase(t) > kMaxNarrow) {
        promote();
        wide_.push_back(t);
        return;
    }
    narrow_.push_back(static_cast<std::uint32_t>(offsetFromBase(t)));
}

void TimeColumn::promote() {
    wide_.reserve(narrow_.size() + 1);
    for (const std::uint32_t off : narrow_)
        wide_.push_back(base_ + static_cast<TimeStamp>(off));
    narrow_.clear();
    narrow_.shrink_to_fit();
}

TimeStamp TimeColumn::operator[](std::size_t i) const noexcept {
    if (!wide_.empty())
        return i < wide_.size() ? wide_[i] : kNoTime;
    return i < narrow_.size() ? base_ + static_cast<TimeStamp>(narrow_[i]) : kNoTime;
}

std::size_t TimeColumn::upperBound(TimeStamp t) const noexcept {
    if (!wide_.empty())
        return static_cast<std::size_t>(std::ranges::upper_bound(wide_, t) - wide_.begin());

    if (narrow_.empty())
        return 0;
    if (t < base_)
        return 0;  // всё правее t
    const std::uint64_t off = offsetFromBase(t);
    if (off > kMaxNarrow)
        return narrow_.size();  // всё левее t
    const auto key = static_cast<std::uint32_t>(off);
    return static_cast<std::size_t>(std::ranges::upper_bound(narrow_, key) - narrow_.begin());
}

std::size_t TimeColumn::lowerBound(TimeStamp t) const noexcept {
    if (!wide_.empty())
        return static_cast<std::size_t>(std::ranges::lower_bound(wide_, t) - wide_.begin());

    if (narrow_.empty())
        return 0;
    if (t <= base_)
        return 0;
    const std::uint64_t off = offsetFromBase(t);
    if (off > kMaxNarrow)
        return narrow_.size();
    const auto key = static_cast<std::uint32_t>(off);
    return static_cast<std::size_t>(std::ranges::lower_bound(narrow_, key) - narrow_.begin());
}

TimeColumn TimeColumn::slice(std::size_t lo, std::size_t hi) const {
    TimeColumn out;
    const std::size_t n = size();
    const std::size_t first = std::min(lo, n);
    const std::size_t last = std::min(hi, n);
    out.narrow_.reserve(last - first);
    for (std::size_t i = first; i < last; ++i)
        out.append((*this)[i]);
    return out;
}

}  // namespace WaSafe
