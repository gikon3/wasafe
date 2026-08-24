#include "base_builder.hpp"

#include <format>

#include "wasafe/core/exception.hpp"

namespace WaSafe {

BaseBuilder::BaseBuilder() {
    stack_.push(hierarchy_.root());
}

void BaseBuilder::setTimeScale(TimeScale scale) {
    scale_ = scale;
}

ScopeId BaseBuilder::beginScope(std::string_view name, ScopeKind kind) {
    const auto id = hierarchy_.addScope(stack_.top(), std::string{name}, kind);
    stack_.push(id);
    return id;
}

void BaseBuilder::endScope() {
    if (stack_.size() > 1)
        stack_.pop();
}

SignalId BaseBuilder::declareVar(std::string_view name, Type type, std::optional<SignalId> alias,
        std::span<SignalId> leaves) {
    // Размер проверяется ДО единого изменения состояния: бросок в середине
    // разворачивания оставил бы часть потоков выделенными, а часть узлов —
    // добавленными в иерархию, и builder стал бы невосстановим.
    if (!leaves.empty() && leaves.size() != expansionStreamCount(type)) {
        throw Exception{std::format("Builder::declareVar('{}'): leaves size {} != expansionStreamCount {}", name,
                leaves.size(), expansionStreamCount(type))};
    }

    const SignalId stream = alias.value_or(makeStream(type));
    const NodeId node = hierarchy_.addSignal(stack_.top(), std::string{name}, type, stream);
    std::size_t pos = 0;
    buildChildren(node, type, stream, 0, leaves, pos);
    return stream;
}

void BaseBuilder::headerDone() {
}

void BaseBuilder::setTime(TimeStamp time) {
    now_ = time;
}

SignalId BaseBuilder::makeStream(const Type& t) {
    if (!singleStreamRepresentable(t))
        return {};  // unpacked-композит собирается из детей; у event/void потока нет
    switch (t->kind()) {
        case TypeKind::REAL:
            return allocStream(ValueKind::REAL, 0);
        case TypeKind::STRING:
            return allocStream(ValueKind::STRING, 0);
        default:  // scalar/vector/enum и packed-композит — один logic-вектор
            return allocStream(ValueKind::LOGIC, t->bitWidth());
    }
}

SignalId BaseBuilder::bindStream(const Type& t, std::span<SignalId> leaves, std::size_t& pos) {
    if (!singleStreamRepresentable(t))
        return {};  // поток этому узлу не положен — курсор не двигаем
    if (leaves.empty())
        return makeStream(t);
    if (pos >= leaves.size()) {
        throw Exception{std::format("Builder::declareVar: expansion needs more streams than the {} provided in leaves",
                leaves.size())};
    }

    SignalId& slot = leaves[pos++];
    if (!slot.valid())
        slot = makeStream(t);  // невалидный вход — выделяем и отдаём наружу
    return slot;               // валидный — привязка к уже существующему потоку
}

// NOLINTNEXTLINE(misc-no-recursion) — разворачивание вложенных композитов по определению рекурсивно
void BaseBuilder::buildChildren(NodeId parent, const Type& type, SignalId owning, std::uint32_t base,
        std::span<SignalId> leaves, std::size_t& pos) {
    if (!type)
        return;
    if (const auto* st = type->as<StructType>()) {
        const bool packed = st->packed();
        for (const StructMember& m : st->members()) {
            const std::uint32_t w = m.type ? m.type->bitWidth() : 0;
            if (packed) {
                const std::uint32_t off = base + m.bitOffset;
                const NodeId child = hierarchy_.addMember(parent, m.name, m.type, SignalId{}, BitSlice{off, w});
                // Внутри packed span не расходуется: биты поддерева покрыты потоком
                // предка, и expansionStreamCount так же считает его закрытым.
                std::size_t nested = 0;
                buildChildren(child, m.type, owning, off, {}, nested);
            }
            else {
                const SignalId cs = bindStream(m.type, leaves, pos);
                const NodeId child = hierarchy_.addMember(parent, m.name, m.type, cs);
                buildChildren(child, m.type, cs, 0, leaves, pos);
            }
        }
    }
    else if (const auto* at = type->as<ArrayType>()) {
        const bool packed = at->packed();
        const Type& elem = at->elementType();
        const std::size_t count = at->elementCount();
        const std::uint32_t ew = elem ? elem->bitWidth() : 0;
        for (std::size_t ord = 0; ord < count; ++ord) {
            const std::int32_t index = at->indexOf(ord);
            if (packed) {
                const std::uint32_t off = base + static_cast<std::uint32_t>(count - 1 - ord) * ew;
                const NodeId child = hierarchy_.addElement(parent, index, elem, SignalId{}, BitSlice{off, ew});
                std::size_t nested = 0;  // см. packed-ветку структуры выше
                buildChildren(child, elem, owning, off, {}, nested);
            }
            else {
                const SignalId cs = bindStream(elem, leaves, pos);
                const NodeId child = hierarchy_.addElement(parent, index, elem, cs);
                buildChildren(child, elem, cs, 0, leaves, pos);
            }
        }
    }
    // лист — детей нет
}

}  // namespace WaSafe
