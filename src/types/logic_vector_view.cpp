#include "wasafe/types/logic_vector_view.hpp"

namespace WaSafe {

bool LogicVectorView::isTwoState() const noexcept {
    if (b_ == nullptr)
        return true;  // двухзначная форма: bval-плана нет, значит все b-биты нулевые
    const std::uint32_t words = wordsFor(width_);
    for (std::uint32_t w = 0; w < words; ++w) {
        if (b_[w] != 0)
            return false;  // любой выставленный b-бит => X или Z
    }
    return true;
}

std::optional<std::uint64_t> LogicVectorView::toUint64() const noexcept {
    if (width_ > kWordWidth || !isTwoState())
        return std::nullopt;

    std::uint64_t out = width_ == 0 ? 0 : a_[0];
    if (width_ < kWordWidth)
        out &= (std::uint64_t{1} << width_) - 1;
    return out;
}

std::string LogicVectorView::toString() const {
    std::string s;
    s.reserve(width_);
    for (std::uint32_t i = width_; i-- > 0;)
        s.push_back(toChar((*this)[i]));
    return s;
}

Logic LogicVectorView::operator[](std::uint32_t bit) const noexcept {
    if (bit >= width_)
        return Logic::X;

    const std::uint32_t word = bit / kWordWidth;
    const std::uint32_t off = bit % kWordWidth;
    const unsigned a = static_cast<unsigned>((a_[word] >> off) & 1u);
    const unsigned b = b_ == nullptr ? 0u : static_cast<unsigned>((b_[word] >> off) & 1u);
    return logicFromAb(a, b);
}

}  // namespace WaSafe
