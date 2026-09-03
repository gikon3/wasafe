#include "storage/bit_planes.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>

namespace WaSafe {

namespace {

using WordType = LogicVectorView::WordType;

constexpr std::uint32_t kWordWidth = LogicVectorView::kWordWidth;
constexpr WordType kAllOnes = std::numeric_limits<WordType>::max();

/// Маска младших bits разрядов слова. bits == 0 читается как «слово целиком».
[[nodiscard]] constexpr WordType lowMask(std::uint32_t bits) noexcept {
    return bits == 0 ? kAllOnes : kAllOnes >> (kWordWidth - bits);
}

/// Скопировать первые words слов плана источника в слот, начинающийся с base.
/// Пустой план источника (двухзначный вид) не копируется вовсе — слова слота
/// уже занулены, и это тот же результат.
///
/// @pre words <= src.size() и base + words <= dst.size().
void copyPlane(std::span<const WordType> src, std::vector<WordType>& dst, std::size_t base,
        std::uint32_t words) noexcept {
    if (!src.empty())
        std::copy_n(src.begin(), words, dst.begin() + static_cast<std::ptrdiff_t>(base));
}

/// Обнулить разряды последнего слова слота за width — инвариант планов, на
/// котором стоят ==, toUint64 и isTwoState. Нужен и после копирования более
/// широкого значения, и после пословного дозаполнения.
void maskTail(std::vector<WordType>& plane, std::size_t base, std::uint32_t words, std::uint32_t width) noexcept {
    plane[base + words - 1] &= lowMask(width % kWordWidth);
}

/// Разряды слота от from и выше — X (a=1, b=1): значение их не записало.
/// Заполнение идёт целыми словами, лишнее за width срежет maskTail. Заодно так
/// перекрывается мусор старших разрядов последнего скопированного слова: чужой
/// вид зануления за своей шириной не обещает.
///
/// @pre bval уже развёрнут — узкое значение его и требует.
void fillUnwritten(std::vector<WordType>& aval, std::vector<WordType>& bval, std::size_t base, std::uint32_t from,
        std::uint32_t words) noexcept {
    for (std::uint32_t w = from / kWordWidth; w < words; ++w) {
        const std::uint32_t lo = w * kWordWidth;
        const WordType fill = lo >= from ? kAllOnes : ~lowMask(from - lo);
        aval[base + w] |= fill;
        bval[base + w] |= fill;
    }
}

}  // namespace

void appendLogicValue(std::uint32_t width, std::vector<WordType>& aval, std::vector<WordType>& bval,
        LogicVectorView src) {
    const std::uint32_t words = LogicVectorView::wordsFor(width);
    const std::size_t base = aval.size();
    aval.resize(base + words, 0);

    // Первое четырёхзначное значение разворачивает план нулями под уже
    // записанные изменения — они были двухзначными, нули верны.
    //
    // Узкое значение тоже требует плана, хотя само двухзначно: незаписанные
    // разряды слота читаются как X, а у него b-бит единичный.
    if (!bval.empty() || !src.isTwoState() || src.width() < width)
        bval.resize(base + words, 0);

    if (words == 0)
        return;

    // Значение покрывает слот целиком: разряды 0..width-1 лежат в тех же
    // разрядах тех же слов, всё лишнее сверху срезает маска хвоста.
    if (src.width() >= width) {
        copyPlane(src.aval(), aval, base, words);
        maskTail(aval, base, words, width);
        if (!bval.empty()) {
            copyPlane(src.bval(), bval, base, words);
            maskTail(bval, base, words, width);
        }
        return;
    }

    // Значение уже слота: копируются только его слова, остаток слота — X.
    copyPlane(src.aval(), aval, base, LogicVectorView::wordsFor(src.width()));
    copyPlane(src.bval(), bval, base, LogicVectorView::wordsFor(src.width()));
    fillUnwritten(aval, bval, base, src.width(), words);
    maskTail(aval, base, words, width);
    maskTail(bval, base, words, width);
}

}  // namespace WaSafe
