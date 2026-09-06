#include "wasafe/model/hierarchy.hpp"

#include <cctype>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include "wasafe/core/exception.hpp"

namespace WaSafe {

namespace {

/// Разбить путь по '.', считая содержимое скобок `[...]` атомарным
/// (точек внутри индексов не бывает, но скобки не разрываем на всякий случай).
std::vector<std::string_view> splitDots(std::string_view path) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '[') {
            ++depth;
        }
        else if (c == ']') {
            if (depth > 0)
                --depth;
        }
        else if (c == '.' && depth == 0) {
            out.push_back(path.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(path.substr(start));
    return out;
}

/// Сегмент пути: базовое имя ("data") + список ключей-индексов ("[3]", "[1]").
/// Ключи нормализованы под имена, которые создаёт Hierarchy::add_element.
struct Segment {
    std::string_view name;             ///< может быть пустым (сегмент вида "[3]")
    std::vector<std::string> indices;  ///< ключи вида "[3]" для member_index
};

Segment parseSegment(std::string_view seg) {
    Segment s;
    const std::size_t br = seg.find('[');
    s.name = seg.substr(0, br);  // при отсутствии '[' substr вернёт весь seg
    if (br == std::string_view::npos)
        return s;
    for (std::size_t i = br; i < seg.size();) {
        if (seg[i] != '[') {
            ++i;
            continue;
        }
        const std::size_t close = seg.find(']', i);
        if (close == std::string_view::npos)
            break;
        std::string_view inner = seg.substr(i + 1, close - i - 1);
        while (!inner.empty() && std::isspace(inner.front()))
            inner.remove_prefix(1);
        while (!inner.empty() && std::isspace(inner.back()))
            inner.remove_suffix(1);
        s.indices.push_back(std::format("[{}]", inner));
        i = close + 1;
    }
    return s;
}

/// Heap, занятый строкой. Короткие строки лежат внутри самого объекта (SSO) и
/// кучу не трогают вовсе; порог SSO не стандартизован, поэтому берётся у пустой
/// строки, а не литералом.
std::size_t stringHeap(const std::string& s) {
    static const std::size_t kSso = std::string{}.capacity();
    return s.capacity() > kSso ? s.capacity() + 1 : 0;  // +1 -- завершающий ноль
}

/// Накладные расходы StringMap БЕЗ heap ключей: они считаются отдельной статьёй
/// (names), иначе длинное имя учлось бы дважды.
///
/// Формула точна для libstdc++: узел -- это указатель на следующий плюс пара
/// (ключ, значение), кэша хеша нет, потому что TransparentStringHash::operator()
/// помечен noexcept. На MSVC список двусвязный, и счёт занижен на указатель с узла.
template <class V>
std::size_t mapOverhead(const StringMap<V>& m) {
    using Node = StringMap<V>::value_type;
    return m.bucket_count() * sizeof(void*) + m.size() * (sizeof(void*) + sizeof(Node));
}

template <class T>
std::size_t vectorHeap(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

}  // namespace

Hierarchy::Hierarchy() {
    ScopeNode root;
    root.kind = ScopeKind::ROOT;
    scopes_.push_back(std::move(root));
}

ScopeId Hierarchy::addScope(ScopeId parent, std::string name, ScopeKind kind) {
    requireScope(parent);
    const auto id = ScopeId{static_cast<ScopeId::ValueType>(scopes_.size())};

    ScopeNode node;
    node.name = std::move(name);
    node.kind = kind;
    node.parent = parent;
    scopes_.push_back(std::move(node));

    auto& p = scopes_[parent.get()];
    p.childScopes.push_back(id);
    p.scopeIndex.emplace(scopes_[id.get()].name, id);
    return id;
}

NodeId Hierarchy::addSignal(ScopeId scope, std::string name, Type type, SignalId stream) {
    requireScope(scope);
    const auto idx = NodeId{static_cast<NodeId::ValueType>(nodes_.size())};

    SignalNode node;
    node.name = std::move(name);
    node.type = std::move(type);
    node.scope = scope;
    node.stream = stream;
    nodes_.push_back(std::move(node));

    auto& s = scopes_[scope.get()];
    s.signals.push_back(idx);
    s.signalIndex.emplace(nodes_[idx.get()].name, idx);
    return idx;
}

NodeId Hierarchy::addMember(NodeId parent, std::string name, Type type, SignalId stream,
        std::optional<BitSlice> projection) {
    requireNode(parent);
    const auto idx = NodeId{static_cast<NodeId::ValueType>(nodes_.size())};

    SignalNode node;
    node.name = std::move(name);
    node.type = std::move(type);
    node.parent = parent;
    node.stream = stream;
    node.projection = projection;
    nodes_.push_back(std::move(node));

    auto& p = nodes_[parent.get()];
    p.children.push_back(idx);
    p.memberIndex.emplace(nodes_[idx.get()].name, idx);
    return idx;
}

NodeId Hierarchy::addElement(NodeId parent, std::int32_t index, Type type, SignalId stream,
        std::optional<BitSlice> projection) {
    return addMember(parent, std::format("[{}]", index), std::move(type), stream, projection);
}

std::size_t Hierarchy::MemoryUse::total() const noexcept {
    return nodes + scopes + names + indexes + children;
}

Hierarchy::MemoryUse Hierarchy::memoryUse() const {
    MemoryUse use;
    // capacity, а не size: векторы узлов растут с запасом, и незаполненный хвост
    // занят так же честно, как заполненный.
    use.nodes = nodes_.capacity() * sizeof(SignalNode);
    use.scopes = scopes_.capacity() * sizeof(ScopeNode);

    for (const auto& s : scopes_) {
        use.names += stringHeap(s.name);
        use.indexes += mapOverhead(s.scopeIndex) + mapOverhead(s.signalIndex);
        use.children += vectorHeap(s.childScopes) + vectorHeap(s.signals);
        // Ключ карты -- НЕЗАВИСИМАЯ копия имени узла (addScope/addSignal кладут
        // его через emplace уже после перемещения в узел), поэтому длинное имя
        // платится дважды, и вторая копия обязана попасть в счёт.
        for (const auto& kv : s.scopeIndex)
            use.names += stringHeap(kv.first);
        for (const auto& kv : s.signalIndex)
            use.names += stringHeap(kv.first);
    }

    for (const auto& n : nodes_) {
        use.names += stringHeap(n.name);
        use.indexes += mapOverhead(n.memberIndex);
        use.children += vectorHeap(n.children);
        for (const auto& kv : n.memberIndex)
            use.names += stringHeap(kv.first);
    }
    return use;
}

std::optional<NodeId> Hierarchy::descend(NodeId node, std::span<const std::string_view> segments) const {
    for (const auto seg : segments) {
        const Segment s = parseSegment(seg);

        // Имя может быть пустым: сегмент вида "[3]" несёт только индексы.
        if (!s.name.empty()) {
            const auto it = nodes_[node.get()].memberIndex.find(s.name);
            if (it == nodes_[node.get()].memberIndex.end())
                return std::nullopt;
            node = it->second;
        }
        for (const auto& key : s.indices) {
            const auto it = nodes_[node.get()].memberIndex.find(key);
            if (it == nodes_[node.get()].memberIndex.end())
                return std::nullopt;
            node = it->second;
        }
    }
    return node;
}

std::optional<NodeId> Hierarchy::findSignal(std::string_view path) const {
    return findSignal(kRootId, path);
}

std::optional<NodeId> Hierarchy::findSignal(NodeId from, std::string_view relative) const {
    requireNode(from);
    if (relative.empty())
        return from;  // «здесь» внутри сигнала — сам сигнал
    return descend(from, splitDots(relative));
}

std::optional<NodeId> Hierarchy::findSignal(ScopeId from, std::string_view relative) const {
    requireScope(from);
    if (relative.empty())
        return std::nullopt;  // пустой путь именует scope, а сигнала здесь нет
    const auto segs = splitDots(relative);

    // Фаза 1: спуск по scope. Полный текст сегмента сопоставляется с дочерним
    // scope (имя scope может содержать скобки, напр. "gen[0]"). Последний сегмент
    // обязан быть сигналом, поэтому в scope его не разрешаем.
    ScopeId scope = from;
    std::size_t idx = 0;
    for (; idx + 1 < segs.size(); ++idx) {
        const auto& sc = scopes_[scope.get()];
        const auto it = sc.scopeIndex.find(segs[idx]);
        if (it == sc.scopeIndex.end())
            break;
        scope = it->second;
    }
    if (idx >= segs.size())
        return std::nullopt;

    // Фаза 2: первый сигнальный сегмент ищется в signalIndex текущего scope.
    const Segment first = parseSegment(segs[idx]);
    const auto& sc = scopes_[scope.get()];
    const auto sit = sc.signalIndex.find(first.name);
    if (sit == sc.signalIndex.end())
        return std::nullopt;

    // Фаза 3: индексы первого сегмента и все следующие сегменты — общий спуск.
    // Хвост "regs[1][2]" без имени сам является сегментом только из индексов,
    // поэтому отдельной ветки под них не нужно.
    const std::string_view firstIndices = segs[idx].substr(first.name.size());
    const auto base = descend(sit->second, {&firstIndices, 1});
    if (!base.has_value())
        return std::nullopt;
    return descend(*base, std::span{segs}.subspan(idx + 1));
}

std::optional<ScopeId> Hierarchy::findScope(std::string_view path) const {
    return findScope(kRootId, path);
}

std::optional<ScopeId> Hierarchy::findScope(ScopeId from, std::string_view relative) const {
    requireScope(from);
    ScopeId scope = from;
    if (relative.empty())
        return scope;

    for (const auto seg : splitDots(relative)) {
        const auto& sc = scopes_[scope.get()];
        const auto it = sc.scopeIndex.find(seg);
        if (it == sc.scopeIndex.end())
            return std::nullopt;
        scope = it->second;
    }

    return scope;
}

std::string Hierarchy::pathOf(NodeId node) const {
    requireNode(node);

    // Собрать локальный путь сигнала снизу вверх до узла верхнего уровня.
    std::vector<std::string_view> parts;
    NodeId n = node;
    ScopeId topScope;
    for (;;) {
        const auto& sn = nodes_[n.get()];
        parts.push_back(sn.name);
        if (!sn.parent.valid()) {
            topScope = sn.scope;
            break;
        }
        n = sn.parent;
    }

    std::string local;
    for (auto part : std::views::reverse(parts)) {
        if (local.empty()) {
            local = std::string{part};
        }
        else if (!part.empty() && part.front() == '[') {
            local += part;  // элемент массива клеится без точки
        }
        else {
            local += '.';
            local += part;
        }
    }

    std::string scopePath = pathOf(topScope);
    if (scopePath.empty())
        return local;
    scopePath += '.';
    scopePath += local;
    return scopePath;
}

std::string Hierarchy::pathOf(ScopeId scope) const {
    requireScope(scope);

    std::vector<std::string_view> names;
    for (ScopeId s = scope; s.valid(); s = scopes_[s.get()].parent) {
        if (s == kRootId)
            break;
        names.push_back(scopes_[s.get()].name);
    }

    return names | std::views::reverse | std::views::join_with('.') | std::ranges::to<std::string>();
}

void Hierarchy::throwInvalidScope(ScopeId id, std::size_t count) {
    throw Exception{std::format("Hierarchy: invalid ScopeId {} (scope count {})", id.get(), count)};
}

void Hierarchy::throwInvalidNode(NodeId node, std::size_t count) {
    throw Exception{std::format("Hierarchy: invalid NodeId {} (node count {})", node.get(), count)};
}

}  // namespace WaSafe
