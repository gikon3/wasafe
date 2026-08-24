#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wasafe/export.hpp"
#include "wasafe/types/logic.hpp"
#include "wasafe/types/logic_vector_view.hpp"

namespace WaSafe {

/// Владеющий вектор 4-значной логики. Мутаторы и хранилище — здесь; read-only
/// операции делегируются LogicVectorView (см. неявное преобразование ниже).
class WASAFE_API LogicVector final {
public:
    using WordType = LogicVectorView::WordType;

    /// Прокси-ссылка на один бит LogicVector: неявно читается как Logic и
    /// присваивается из Logic — запись затрагивает оба бит-плана (aval/bval).
    /// Создаётся только LogicVector::operator[]; валидна, пока жив сам вектор.
    class WASAFE_API LogicRef final {
        friend class LogicVector;

    public:
        LogicRef(const LogicRef&) = default;
        LogicRef(LogicRef&&) = default;
        ~LogicRef() = default;

        /// Инвертировать бит на месте (IEEE 1164 НЕ): 0↔1, X/Z→X.
        LogicRef& flip() noexcept { return *this = logicNot(static_cast<Logic>(*this)); }

        // Присваивание из другого прокси пишет ЗНАЧЕНИЕ (а не переуказывает ссылку).
        LogicRef& operator=(const LogicRef& other) noexcept {
            if (this != &other)
                *this = static_cast<Logic>(other);
            return *this;
        }

        LogicRef& operator=(LogicRef&& other) noexcept { return *this = static_cast<Logic>(other); }

        LogicRef& operator=(Logic v) noexcept {
            if (logicA(v))
                *a_ |= mask_;
            else
                *a_ &= ~mask_;
            if (logicB(v))
                *b_ |= mask_;
            else
                *b_ &= ~mask_;
            return *this;
        }

        operator Logic() const noexcept {
            const unsigned a = (*a_ & mask_) ? 1u : 0u;
            const unsigned b = (*b_ & mask_) ? 1u : 0u;
            return logicFromAb(a, b);
        }

        [[nodiscard]] Logic operator~() const noexcept { return logicNot(static_cast<Logic>(*this)); }

    private:
        LogicRef(WordType* avalWord, WordType* bvalWord, WordType mask) noexcept :
                a_{avalWord}, b_{bvalWord}, mask_{mask} {}

    private:
        WordType* a_;
        WordType* b_;
        WordType mask_;
    };

public:
    LogicVector() = default;
    explicit LogicVector(std::uint32_t width) :
            a_(LogicVectorView::wordsFor(width), 0), b_(LogicVectorView::wordsFor(width), 0), width_{width} {}
    explicit LogicVector(LogicVectorView v) : width_{v.width()} {
        a_.assign(v.aval().begin(), v.aval().end());
        // Двухзначная вьюха отдаёт пустой bval — разворачиваем его в нули, иначе
        // планы разъедутся по длине и сломают инварианты (==, clearTailBits, запись).
        if (const auto b = v.bval(); !b.empty())
            b_.assign(b.begin(), b.end());
        else
            b_.assign(LogicVectorView::wordsFor(width_), 0);
    }

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return width_; }  ///< синоним width() (dynamic_bitset)
    [[nodiscard]] bool empty() const noexcept { return width_ == 0; }

    // --- модификаторы (в духе boost::dynamic_bitset) ------------------------
    /// Установить бит с проверкой границ; бросает Exception при bit >= size().
    void set(std::uint32_t bit, Logic v = Logic::ONE);
    void set() noexcept;            ///< все биты -> 1
    void reset() noexcept;          ///< все биты -> 0
    void reset(std::uint32_t bit);  ///< бит -> 0 (с проверкой границ)
    void flip() noexcept;           ///< инвертировать все биты (IEEE 1164 НЕ)
    void flip(std::uint32_t bit);   ///< инвертировать бит (с проверкой границ)

    void pushBack(Logic v);                                     ///< дописать бит в конец
    void popBack() noexcept;                                    ///< убрать последний бит (no-op, если пусто)
    void resize(std::uint32_t bits, Logic fill = Logic::ZERO);  ///< изменить размер
    void clear() noexcept {
        a_.clear();
        b_.clear();
        width_ = 0;
    }

    /// Заполнить из строки логических символов (MSB слева), напр. "10xz".
    void assignFromChars(std::string_view chars);
    void assignFromUint64(std::uint64_t value);

    [[nodiscard]] std::span<const WordType> aval() const noexcept { return a_; }
    [[nodiscard]] std::span<const WordType> bval() const noexcept { return b_; }

    [[nodiscard]] Logic get(std::uint32_t bit) const noexcept { return view()[bit]; }
    [[nodiscard]] bool isTwoState() const noexcept { return view().isTwoState(); }
    [[nodiscard]] std::string toString() const { return view().toString(); }
    [[nodiscard]] std::optional<std::uint64_t> toUint64() const noexcept { return view().toUint64(); }

    [[nodiscard]] Logic operator[](std::uint32_t bit) const noexcept { return view()[bit]; }
    [[nodiscard]] Logic test(std::uint32_t bit) const;  ///< чтение с проверкой границ (бросает Exception)

    [[nodiscard]] bool operator==(const LogicVector& other) const noexcept {
        return width_ == other.width_ && a_ == other.a_ && b_ == other.b_;
    }

    /// Запись одного бита через прокси: `vec[bit] = Logic::ONE`. БЕЗ проверки
    /// границ (предусловие: bit < width()); проверяемая запись — set().
    LogicRef operator[](std::uint32_t bit) noexcept {
        const std::uint32_t word = bit / LogicVectorView::kWordWidth;
        const WordType mask = WordType{1} << (bit % LogicVectorView::kWordWidth);
        return LogicRef{&a_[word], &b_[word], mask};
    }

    operator LogicVectorView() const noexcept { return view(); }

private:
    [[nodiscard]] LogicVectorView view() const noexcept { return {a_.data(), b_.data(), width_}; }

    /// Обнулить неиспользуемые старшие разряды последнего слова (инвариант:
    /// биты за пределами width() всегда нули — нужно для ==, toUint64 и т.п.).
    void clearTailBits() noexcept {
        if (a_.empty())
            return;
        const std::uint32_t rem = width_ % LogicVectorView::kWordWidth;
        if (rem == 0)
            return;
        const WordType mask = (WordType{1} << rem) - 1;
        a_.back() &= mask;
        b_.back() &= mask;
    }

private:
    std::vector<WordType> a_;
    std::vector<WordType> b_;
    std::uint32_t width_ = 0;
};

}  // namespace WaSafe
