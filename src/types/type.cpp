#include "wasafe/types/type.hpp"

#include <algorithm>

namespace WaSafe {

std::string_view toString(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::SCALAR:
            return "scalar";
        case TypeKind::VECTOR:
            return "vector";
        case TypeKind::ARRAY:
            return "array";
        case TypeKind::STRUCT:
            return "struct";
        case TypeKind::UNION:
            return "union";
        case TypeKind::ENUM:
            return "enum";
        case TypeKind::REAL:
            return "real";
        case TypeKind::STRING:
            return "string";
        case TypeKind::EVENT:
            return "event";
        case TypeKind::VOID:
            return "void";
    }
    return "?";
}

// --- StructType -------------------------------------------------------------
std::optional<std::size_t> StructType::indexOf(std::string_view memberName) const noexcept {
    for (std::size_t i = 0; i < members_.size(); ++i) {
        if (members_[i].name == memberName)
            return i;
    }
    return std::nullopt;
}

std::uint32_t StructType::bitWidth() const noexcept {
    if (!packed_)
        return 0;  // распакованная структура не имеет единого вектора
    std::uint32_t total = 0;
    for (const auto& m : members_)
        total += m.type ? m.type->bitWidth() : 0;
    return total;
}

bool StructType::computeFourState(const std::vector<StructMember>& m) {
    return std::ranges::any_of(m, [](const StructMember& mem) { return mem.type && mem.type->fourState(); });
}

// --- EnumType ---------------------------------------------------------------
std::optional<std::string_view> EnumType::labelOf(std::uint64_t value) const noexcept {
    for (const auto& e : entries_) {
        if (e.value == value)
            return e.name;
    }
    return std::nullopt;
}

// --- Фабрики (интернирование — TODO; пока просто создают объекты) -----------
Type makeScalar(bool fourState) {
    return std::make_shared<const ScalarType>(fourState);
}
Type makeVector(std::int32_t msb, std::int32_t lsb, bool isSigned, bool fourState) {
    return std::make_shared<const VectorType>(msb, lsb, isSigned, fourState);
}
Type makeArray(Type element, std::int32_t left, std::int32_t right, bool packed) {
    return std::make_shared<const ArrayType>(std::move(element), left, right, packed);
}
Type makeStruct(std::vector<StructMember> members, bool packed) {
    return std::make_shared<const StructType>(std::move(members), packed, false);
}
Type makeReal(bool shortreal) {
    return std::make_shared<const RealType>(shortreal);
}
Type makeString() {
    return std::make_shared<const StringType>();
}

bool singleStreamRepresentable(const Type& t) {
    if (!t)
        return false;
    switch (t->kind()) {
        case TypeKind::SCALAR:
        case TypeKind::VECTOR:
        case TypeKind::ENUM:
        case TypeKind::REAL:
        case TypeKind::STRING:
            return true;
        case TypeKind::STRUCT:
        case TypeKind::UNION: {
            const auto* st = t->as<StructType>();
            return st != nullptr && st->packed();
        }
        case TypeKind::ARRAY: {
            const auto* at = t->as<ArrayType>();
            return at != nullptr && at->packed();
        }
        default:
            return false;  // event/void — потока нет вовсе
    }
}

namespace {

/// Сколько потоков выделяется на этот тип ЦЕЛИКОМ: свой поток либо потоки его
/// поддерева. Ноль у event/void — им поток не положен.
// NOLINTNEXTLINE(misc-no-recursion) — обход композита по определению рекурсивен
std::size_t streamsOf(const Type& t) {
    return singleStreamRepresentable(t) ? 1 : expansionStreamCount(t);
}

}  // namespace

// NOLINTNEXTLINE(misc-no-recursion) — обход композита по определению рекурсивен
std::size_t expansionStreamCount(const Type& t) {
    if (!t || singleStreamRepresentable(t))
        return 0;  // свой поток покрывает всё поддерево — разворачивание не выделяет ничего

    std::size_t total = 0;
    if (const auto* st = t->as<StructType>()) {
        for (const StructMember& m : st->members())
            total += streamsOf(m.type);
    }
    else if (const auto* at = t->as<ArrayType>()) {
        total = at->elementCount() * streamsOf(at->elementType());
    }
    return total;
}

std::string toSvString(const Type& t) {
    if (!t)
        return "void";
    // TODO(impl): рекурсивно собрать SV-нотацию (logic [7:0] [0:3] {...}).
    return std::string(toString(t->kind()));
}

}  // namespace WaSafe
