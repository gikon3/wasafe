#include "wasafe/types/logic_vector.hpp"

#include <algorithm>
#include <format>

#include "wasafe/core/exception.hpp"

namespace WaSafe {

void LogicVector::set(std::uint32_t bit, Logic v) {
    if (bit >= width_)
        throw Exception{std::format("LogicVector::set: bit {} out of range (width {})", bit, width_)};
    (*this)[bit] = v;
}

void LogicVector::set() noexcept {
    std::ranges::fill(a_, ~WordType{0});
    std::ranges::fill(b_, WordType{0});
    clearTailBits();
}

void LogicVector::reset() noexcept {
    std::ranges::fill(a_, WordType{0});
    std::ranges::fill(b_, WordType{0});
}

void LogicVector::reset(std::uint32_t bit) {
    set(bit, Logic::ZERO);
}

void LogicVector::flip() noexcept {
    for (std::uint32_t i = 0; i < width_; ++i)
        (*this)[i].flip();
}

void LogicVector::flip(std::uint32_t bit) {
    if (bit >= width_)
        throw Exception{std::format("LogicVector::flip: bit {} out of range (width {})", bit, width_)};
    (*this)[bit].flip();
}

Logic LogicVector::test(std::uint32_t bit) const {
    if (bit >= width_)
        throw Exception{std::format("LogicVector::test: bit {} out of range (width {})", bit, width_)};
    return view()[bit];
}

void LogicVector::pushBack(Logic v) {
    const std::uint32_t bit = width_;
    ++width_;
    a_.resize(LogicVectorView::wordsFor(width_), 0);
    b_.resize(LogicVectorView::wordsFor(width_), 0);
    (*this)[bit] = v;
}

void LogicVector::popBack() noexcept {
    if (width_ == 0)
        return;
    --width_;
    a_.resize(LogicVectorView::wordsFor(width_), 0);
    b_.resize(LogicVectorView::wordsFor(width_), 0);
    clearTailBits();  // обнулить разряд за новым размером в последнем слове
}

void LogicVector::resize(std::uint32_t bits, Logic fill) {
    const std::uint32_t old = width_;
    a_.resize(LogicVectorView::wordsFor(bits), 0);
    b_.resize(LogicVectorView::wordsFor(bits), 0);
    width_ = bits;
    if (bits > old) {
        if (fill != Logic::ZERO)  // новые слова уже занулены — заполняем лишь при fill != 0
            for (std::uint32_t i = old; i < bits; ++i)
                (*this)[i] = fill;
    }
    else {
        clearTailBits();  // отбросить разряды за новым размером
    }
}

void LogicVector::assignFromChars(std::string_view chars) {
    // chars: MSB слева. Бит 0 — последний символ.
    const std::uint32_t n = std::min<std::uint32_t>(width_, static_cast<std::uint32_t>(chars.size()));
    for (std::uint32_t i = 0; i < n; ++i)
        set(i, logicFromChar(chars[chars.size() - 1 - i]));
}

void LogicVector::assignFromUint64(std::uint64_t value) {
    for (std::uint32_t i = 0; i < width_ && i < LogicVectorView::kWordWidth; ++i)
        set(i, (value >> i) & 1u ? Logic::ONE : Logic::ZERO);
}

}  // namespace WaSafe
