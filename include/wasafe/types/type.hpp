#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wasafe/export.hpp"

namespace WaSafe {

class TypeDescriptor;

/// Тип хранится как разделяемый неизменяемый дескриптор: множество сигналов
/// одного SV-типа ссылаются на один объект (интернирование экономит память).
using Type = std::shared_ptr<const TypeDescriptor>;

/// Категория типа.
enum class TypeKind : std::uint8_t {
    SCALAR,  ///< одиночный бит (bit/logic)
    VECTOR,  ///< упакованный диапазон [msb:lsb]
    ARRAY,   ///< распакованный или упакованный массив одного измерения
    STRUCT,  ///< структура с именованными членами
    UNION,   ///< объединение
    ENUM,    ///< перечисление поверх базового типа
    REAL,    ///< real/shortreal
    STRING,  ///< строковый тип
    EVENT,   ///< event
    VOID,
};

[[nodiscard]] WASAFE_API std::string_view toString(TypeKind k) noexcept;

/// Базовый, неизменяемый дескриптор типа. Конкретные виды — наследники ниже.
/// Доступ к производным — через kind() + as<...>() (безопасный down-cast).
class WASAFE_API TypeDescriptor {
public:
    virtual ~TypeDescriptor() = default;

    [[nodiscard]] TypeKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool fourState() const noexcept { return fourState_; }

    /// Полная ширина в битах для упакованного представления; 0 — неприменимо
    /// (real/string/event/распакованные агрегаты без единого вектора).
    [[nodiscard]] virtual std::uint32_t bitWidth() const noexcept = 0;

    /// Число прямых дочерних элементов (членов структуры / элементов массива);
    /// 0 для скаляров/векторов/листовых типов.
    [[nodiscard]] virtual std::size_t elementCount() const noexcept { return 0; }

    // Удобные предикаты.
    [[nodiscard]] bool isScalar() const noexcept { return kind_ == TypeKind::SCALAR; }
    [[nodiscard]] bool isVector() const noexcept { return kind_ == TypeKind::VECTOR; }
    [[nodiscard]] bool isArray() const noexcept { return kind_ == TypeKind::ARRAY; }
    [[nodiscard]] bool isStruct() const noexcept { return kind_ == TypeKind::STRUCT; }
    [[nodiscard]] bool isUnion() const noexcept { return kind_ == TypeKind::UNION; }
    [[nodiscard]] bool isComposite() const noexcept { return elementCount() > 0; }

    /// Безопасное приведение к производному типу (nullptr при несовпадении).
    template <class T>
    [[nodiscard]] const T* as() const noexcept {
        return dynamic_cast<const T*>(this);
    }

protected:
    TypeDescriptor(TypeKind k, bool fourState) : kind_{k}, fourState_{fourState} {}
    TypeDescriptor(const TypeDescriptor&) = default;
    TypeDescriptor(TypeDescriptor&&) = default;

    TypeDescriptor& operator=(const TypeDescriptor&) = default;
    TypeDescriptor& operator=(TypeDescriptor&&) = default;

private:
    TypeKind kind_;
    bool fourState_;
};

// ---------------------------------------------------------------------------
// Скаляр: один бит.
// ---------------------------------------------------------------------------
class WASAFE_API ScalarType final : public TypeDescriptor {
public:
    explicit ScalarType(bool fourState = true) : TypeDescriptor{TypeKind::SCALAR, fourState} {}
    [[nodiscard]] std::uint32_t bitWidth() const noexcept override { return 1; }
};

// ---------------------------------------------------------------------------
// Вектор: упакованный диапазон бит [msb:lsb], знаковость.
// ---------------------------------------------------------------------------
class WASAFE_API VectorType final : public TypeDescriptor {
public:
    VectorType(std::int32_t msb, std::int32_t lsb, bool isSigned = false, bool fourState = true) :
            TypeDescriptor{TypeKind::VECTOR, fourState}, msb_{msb}, lsb_{lsb}, signed_{isSigned} {}

    [[nodiscard]] std::int32_t msb() const noexcept { return msb_; }
    [[nodiscard]] std::int32_t lsb() const noexcept { return lsb_; }
    [[nodiscard]] bool isSigned() const noexcept { return signed_; }
    [[nodiscard]] std::uint32_t bitWidth() const noexcept override {
        return static_cast<std::uint32_t>((msb_ >= lsb_ ? msb_ - lsb_ : lsb_ - msb_) + 1);
    }

private:
    std::int32_t msb_, lsb_;
    bool signed_;
};

// ---------------------------------------------------------------------------
// Массив (одно измерение). Многомерность выражается вложением:
//   logic [7:0] m [0:3][0:1]  =>  Array{0:3, Array{0:1, Vector[7:0]}}
// ---------------------------------------------------------------------------
class WASAFE_API ArrayType final : public TypeDescriptor {
public:
    ArrayType(Type element, std::int32_t indexLeft, std::int32_t indexRight, bool packed = false) :
            TypeDescriptor{TypeKind::ARRAY, element ? element->fourState() : true}, element_{std::move(element)},
            left_{indexLeft}, right_{indexRight}, packed_{packed} {}

    [[nodiscard]] const Type& elementType() const noexcept { return element_; }
    [[nodiscard]] std::int32_t indexLeft() const noexcept { return left_; }
    [[nodiscard]] std::int32_t indexRight() const noexcept { return right_; }
    [[nodiscard]] bool packed() const noexcept { return packed_; }

    /// Арифметика краёв — в int64: разность двух int32 переполняет знаковый тип
    /// на вырожденных диапазонах вроде [INT32_MAX : INT32_MIN], а это UB.
    [[nodiscard]] std::size_t elementCount() const noexcept override {
        const std::int64_t span =
                left_ >= right_ ? static_cast<std::int64_t>(left_) - right_ : static_cast<std::int64_t>(right_) - left_;
        return static_cast<std::size_t>(span) + 1;
    }

    /// Преобразование логического индекса массива в порядковый (0..count-1);
    /// nullopt, если индекс не принадлежит диапазону массива. Отдельная проверка
    /// вхождения не нужна: попадание порядкового в [0, count) ей равносильно.
    [[nodiscard]] std::optional<std::size_t> ordinalOf(std::int32_t index) const noexcept {
        const std::int64_t ordinal =
                left_ >= right_ ? static_cast<std::int64_t>(left_) - index : static_cast<std::int64_t>(index) - left_;
        if (ordinal < 0 || static_cast<std::uint64_t>(ordinal) >= static_cast<std::uint64_t>(elementCount()))
            return std::nullopt;
        return static_cast<std::size_t>(ordinal);
    }

    /// Обратное преобразование; nullopt, если порядковый вне числа элементов.
    /// После проверки результат гарантированно лежит между краями, то есть в int32.
    [[nodiscard]] std::optional<std::int32_t> indexOf(std::size_t ordinal) const noexcept {
        if (ordinal >= elementCount())
            return std::nullopt;
        const std::int64_t off = static_cast<std::int64_t>(ordinal);
        return static_cast<std::int32_t>(left_ >= right_ ? left_ - off : left_ + off);
    }

    [[nodiscard]] std::uint32_t bitWidth() const noexcept override {
        return packed_ && element_ ? element_->bitWidth() * static_cast<std::uint32_t>(elementCount()) : 0;
    }

private:
    Type element_;
    std::int32_t left_, right_;
    bool packed_;
};

// ---------------------------------------------------------------------------
// Структура / объединение: упорядоченные именованные члены.
// ---------------------------------------------------------------------------
struct StructMember {
    std::string name;
    Type type;
    std::uint32_t bitOffset = 0;  ///< смещение в битах внутри packed-представления
};

class WASAFE_API StructType final : public TypeDescriptor {
public:
    StructType(std::vector<StructMember> members, bool packed, bool isUnion = false) :
            TypeDescriptor{isUnion ? TypeKind::UNION : TypeKind::STRUCT, computeFourState(members)},
            members_{std::move(members)}, packed_{packed} {}

    [[nodiscard]] std::span<const StructMember> members() const noexcept { return members_; }
    [[nodiscard]] bool packed() const noexcept { return packed_; }
    [[nodiscard]] std::size_t elementCount() const noexcept override { return members_.size(); }

    /// Поиск члена по имени; nullopt, если члена с таким именем нет.
    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view memberName) const noexcept;

    [[nodiscard]] std::uint32_t bitWidth() const noexcept override;

private:
    static bool computeFourState(const std::vector<StructMember>& m);
    std::vector<StructMember> members_;
    bool packed_;
};

// ---------------------------------------------------------------------------
// Перечисление поверх базового (обычно векторного) типа.
// ---------------------------------------------------------------------------
struct EnumEntry {
    std::string name;
    std::uint64_t value = 0;
};

class WASAFE_API EnumType final : public TypeDescriptor {
public:
    EnumType(Type base, std::vector<EnumEntry> entries) :
            TypeDescriptor{TypeKind::ENUM, base ? base->fourState() : true}, base_{std::move(base)},
            entries_{std::move(entries)} {}

    [[nodiscard]] const Type& base() const noexcept { return base_; }
    [[nodiscard]] std::span<const EnumEntry> entries() const noexcept { return entries_; }
    [[nodiscard]] std::uint32_t bitWidth() const noexcept override { return base_ ? base_->bitWidth() : 0; }
    /// Символьное имя для значения; nullopt, если константы с таким значением нет.
    /// Пустой string_view — это метка с пустым именем, а не её отсутствие.
    [[nodiscard]] std::optional<std::string_view> labelOf(std::uint64_t value) const noexcept;

private:
    Type base_;
    std::vector<EnumEntry> entries_;
};

// ---------------------------------------------------------------------------
// Скалярные «листовые» типы без бит-ширины.
// ---------------------------------------------------------------------------
class WASAFE_API RealType final : public TypeDescriptor {
public:
    explicit RealType(bool shortreal = false) : TypeDescriptor{TypeKind::REAL, false}, short_{shortreal} {}
    [[nodiscard]] bool isShortreal() const noexcept { return short_; }
    [[nodiscard]] std::uint32_t bitWidth() const noexcept override { return 0; }

private:
    bool short_;
};

class WASAFE_API StringType final : public TypeDescriptor {
public:
    explicit StringType() : TypeDescriptor{TypeKind::STRING, false} {}
    [[nodiscard]] std::uint32_t bitWidth() const noexcept override { return 0; }
};

// ---------------------------------------------------------------------------
// Фабрики (возвращают интернируемые Type). Реализация — в types/type.cpp.
// ---------------------------------------------------------------------------
[[nodiscard]] WASAFE_API Type makeScalar(bool fourState = true);
[[nodiscard]] WASAFE_API Type makeVector(std::int32_t msb, std::int32_t lsb, bool isSigned = false,
        bool fourState = true);
[[nodiscard]] WASAFE_API Type makeArray(Type element, std::int32_t left, std::int32_t right, bool packed = false);
[[nodiscard]] WASAFE_API Type makeStruct(std::vector<StructMember> members, bool packed);
[[nodiscard]] WASAFE_API Type makeReal(bool shortreal = false);
[[nodiscard]] WASAFE_API Type makeString();

/// Полное текстовое представление типа в SV-нотации ("logic [7:0]", "struct packed {...}").
[[nodiscard]] WASAFE_API std::string toSvString(const Type& t);

// ---------------------------------------------------------------------------
// Представимость типа потоком значений. Правило одно на всю библиотеку: им
// пользуется и Builder при разворачивании композита, и внешний формат, которому
// нужно знать размер span явной привязки потоков (см. Builder::declareVar).
// ---------------------------------------------------------------------------

/// Тип представим ОДНИМ потоком значений: лист (scalar/vector/enum/real/string)
/// либо packed-композит, члены которого — битовые срезы этого потока.
[[nodiscard]] WASAFE_API bool singleStreamRepresentable(const Type& t);

/// Число потоков, которые declareVar выделит при РАЗВОРАЧИВАНИИ детей этого
/// типа, — и требуемый размер span leaves.
///
/// 0, если разворачивание не выделяет потоков: тип представим одним потоком
/// (лист или packed-композит) либо не имеет потока вовсе (event/void/пустой
/// Type). Ноль поэтому НЕ эквивалентен singleStreamRepresentable() == true.
[[nodiscard]] WASAFE_API std::size_t expansionStreamCount(const Type& t);

}  // namespace WaSafe
