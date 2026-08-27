#include "wasafe/types/logic_vector_view.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "wasafe/core/exception.hpp"

namespace WaSafe {

namespace {

/// Символы разрядов; их количество и задаёт потолок бит на разряд.
constexpr std::string_view kDigits = "0123456789abcdef";

/// Разряд из группы бит [lo, lo + count) по правилу IEEE 1800: вся группа из
/// 0/1 — цифра; вся из одного неопределённого значения — его символ; смешанная
/// (в том числе «определённые пополам с неопределёнными») — всегда 'x'.
///
/// @pre lo + count <= v.width(); count ограничен вместимостью kDigits — за это
///      отвечает groupedString, который count и вычисляет.
char groupDigit(const LogicVectorView& v, std::uint32_t lo, std::uint32_t count) noexcept {
    unsigned value = 0;
    bool anyDefined = false;
    bool anyX = false;
    bool anyZ = false;

    for (std::uint32_t i = 0; i < count; ++i) {
        switch (v[lo + i]) {
            case Logic::ZERO:
                anyDefined = true;
                break;
            case Logic::ONE:
                anyDefined = true;
                value |= 1u << i;
                break;
            case Logic::X:
                anyX = true;
                break;
            case Logic::Z:
                anyZ = true;
                break;
        }
    }

    if (!anyX && !anyZ)
        return kDigits[value];
    if (!anyDefined && !anyZ)
        return 'x';
    if (!anyDefined && !anyX)
        return 'z';
    return 'x';  // смешанная группа неразличима по разрядам
}

/// Слово aval с обнулённым хвостом за width(). Владеющий LogicVector этот
/// инвариант держит сам, но вьюха может прийти из чужого источника.
std::uint64_t maskedWord(const LogicVectorView& v, std::uint32_t index) noexcept {
    const std::uint64_t word = v.aval()[index];
    const std::uint32_t high = (index + 1) * LogicVectorView::kWordWidth;
    if (high <= v.width())
        return word;
    const std::uint32_t bits = v.width() - index * LogicVectorView::kWordWidth;  // 1..63
    return word & ((std::uint64_t{1} << bits) - 1);
}

/// Модуль значения, разложенный в 32-битные лаймы (младший первым). Для
/// отрицательного знакового — модуль дополнения до двух по width().
std::vector<std::uint32_t> magnitudeLimbs(const LogicVectorView& v, bool negative) {
    const std::uint32_t words = LogicVectorView::wordsFor(v.width());
    std::vector<std::uint64_t> value;
    value.reserve(words);
    for (std::uint32_t i = 0; i < words; ++i)
        value.push_back(maskedWord(v, i));

    if (negative) {
        // -x == ~x + 1 по модулю 2^width: дополнение выставляет и биты за
        // width, поэтому хвост гасится до сложения и после переноса.
        const std::uint32_t tail = v.width() % LogicVectorView::kWordWidth;
        const std::uint64_t topMask = tail == 0 ? ~std::uint64_t{0} : (std::uint64_t{1} << tail) - 1;
        for (auto& word : value)
            word = ~word;
        value.back() &= topMask;
        for (auto& word : value) {
            if (++word != 0)
                break;  // переноса дальше нет
        }
        value.back() &= topMask;
    }

    std::vector<std::uint32_t> limbs;
    limbs.reserve(value.size() * 2);
    for (const std::uint64_t word : value) {
        limbs.push_back(static_cast<std::uint32_t>(word));
        limbs.push_back(static_cast<std::uint32_t>(word >> 32));
    }
    return limbs;
}

/// Десятичная запись числа, заданного 32-битными лаймами. Деление столбиком на
/// 10^9: остаток меньше 2^30, поэтому (rem << 32) | limb укладывается в 64 бита
/// и 128-битная арифметика не нужна.
std::string decimalDigits(std::vector<std::uint32_t> limbs) {
    constexpr std::uint32_t kChunk = 1'000'000'000u;
    constexpr int kChunkDigits = 9;

    const auto dropLeadingZeros = [&limbs] {
        while (!limbs.empty() && limbs.back() == 0)
            limbs.pop_back();
    };

    dropLeadingZeros();
    if (limbs.empty())
        return "0";

    std::string out;
    while (!limbs.empty()) {
        std::uint64_t rem = 0;
        for (std::size_t i = limbs.size(); i-- > 0;) {
            const std::uint64_t cur = (rem << 32) | limbs[i];
            limbs[i] = static_cast<std::uint32_t>(cur / kChunk);
            rem = cur % kChunk;
        }
        dropLeadingZeros();

        // Полные 9 цифр, пока сверху что-то осталось; у старшей порции ведущие
        // нули не нужны.
        for (int d = 0; d < kChunkDigits; ++d) {
            out.push_back(static_cast<char>('0' + rem % 10));
            rem /= 10;
            if (limbs.empty() && rem == 0)
                break;
        }
    }

    std::ranges::reverse(out);
    return out;
}

/// Представление группировкой бит: kBitsPerDigit бит на разряд, старший слева.
///
/// Число бит на разряд — параметр ШАБЛОНА, а не значение: тогда проверка
/// вместимости kDigits привязана к инстанцированию и её нельзя забыть. Новое
/// основание в Radix обязано добавить case в toString, а тот инстанцирует эту
/// функцию — и если таблица разрядов не тянет, сборка падает здесь же.
template <std::uint32_t kBitsPerDigit>
std::string groupedString(const LogicVectorView& v) {
    static_assert(kBitsPerDigit > 0);
    static_assert((std::size_t{1} << kBitsPerDigit) <= kDigits.size(),
            "kDigits не адресует столько бит на разряд — расширьте таблицу символов");

    const std::uint32_t width = v.width();
    const std::uint32_t digits = (width + kBitsPerDigit - 1) / kBitsPerDigit;

    std::string s;
    s.reserve(digits);
    for (std::uint32_t d = digits; d-- > 0;) {
        const std::uint32_t lo = d * kBitsPerDigit;
        // Старшая группа может быть неполной; count <= kBitsPerDigit, а значит
        // индекс в kDigits укладывается — это и проверено static_assert выше.
        s.push_back(groupDigit(v, lo, std::min(kBitsPerDigit, width - lo)));
    }
    return s;
}

/// Десятичное представление. Неопределённость в десятичном разряде не
/// локализуется, поэтому любой x/z делает число непредставимым целиком.
std::string decimalString(const LogicVectorView& v, bool isSigned) {
    if (!v.isTwoState())
        return "x";

    const bool negative = isSigned && v[v.width() - 1] == Logic::ONE;
    std::string s = decimalDigits(magnitudeLimbs(v, negative));
    if (negative)
        s.insert(s.begin(), '-');
    return s;
}

}  // namespace

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

std::string LogicVectorView::toString(Radix radix, bool isSigned) const {
    if (width_ == 0)
        return {};  // значения нет ни в одном основании

    // switch без default: новое основание даст -Wswitch, а его case обязан
    // инстанцировать groupedString<N> — там и стоит проверка вместимости kDigits.
    switch (radix) {
        case Radix::BIN:
            return groupedString<1>(*this);
        case Radix::OCT:
            return groupedString<3>(*this);
        case Radix::HEX:
            return groupedString<4>(*this);
        case Radix::DEC:
            return decimalString(*this, isSigned);
    }

    // Сюда приводит только основание вне Radix — приведённый мусор у вызывающей
    // стороны либо забытый выше case.
    throw Exception{std::format("LogicVectorView::toString: unknown radix {}", static_cast<int>(radix))};
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
