#include "wasafe/types/logic_vector.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <span>

#include "wasafe/core/exception.hpp"

namespace WaSafe {

namespace {

using WordType = LogicVector::WordType;

/// Слово плана; за концом плана — ноль. Ноль верен для обоих случаев выхода:
/// двухзначный вид bval не хранит вовсе, а слова за шириной источника всё равно
/// перезапишет заполнение X ниже — его же чужой вид и не обязан занулять.
[[nodiscard]] WordType planeWord(std::span<const WordType> plane, std::size_t index) noexcept {
    return index < plane.size() ? plane[index] : 0;
}

}  // namespace

LogicVector LogicVector::fromSlice(LogicVectorView src, std::uint32_t offset, std::uint32_t width) {
    LogicVector out{width};
    if (width == 0)
        return out;

    const std::span<const WordType> a = src.aval();
    const std::span<const WordType> b = src.bval();  // пуст у двухзначного вида
    const std::uint32_t words = LogicVectorView::wordsFor(width);
    const std::size_t first = offset / LogicVectorView::kWordWidth;
    const std::uint32_t shift = offset % LogicVectorView::kWordWidth;

    for (std::uint32_t k = 0; k < words; ++k) {
        const std::size_t w = first + k;
        out.a_[k] = planeWord(a, w) >> shift;
        out.b_[k] = planeWord(b, w) >> shift;
        if (shift != 0) {
            const std::uint32_t back = LogicVectorView::kWordWidth - shift;
            out.a_[k] |= planeWord(a, w + 1) << back;
            out.b_[k] |= planeWord(b, w + 1) << back;
        }
    }

    // За шириной источника верный ответ — X (a=1, b=1): «биты не записаны».
    // Присваивание, а не ИЛИ, поэтому мусор старших разрядов чужого вида сюда и
    // не протекает. Цикл поразрядный намеренно: он работает лишь на срезе,
    // выходящем за источник, то есть в редком случае.
    const std::uint32_t have = offset < src.width() ? std::min(width, src.width() - offset) : 0;
    for (std::uint32_t i = have; i < width; ++i)
        out[i] = Logic::X;

    out.clearTailBits();
    return out;
}

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
