#include "wasafe/model/hierarchy.hpp"

#include <cctype>
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

std::optional<NodeId> Hierarchy::findSignal(std::string_view path) const {
    if (path.empty())
        return std::nullopt;
    const auto segs = splitDots(path);

    // Фаза 1: спуск по scope. Полный текст сегмента сопоставляется с дочерним
    // scope (имя scope может содержать скобки, напр. "gen[0]"). Последний сегмент
    // обязан быть сигналом, поэтому в scope его не разрешаем.
    ScopeId scope = kRootId;
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

    // Фаза 2: первый сигнальный сегмент ищется в signal_index текущего scope,
    // затем его индексы — в member_index (элементы массива).
    const Segment first = parseSegment(segs[idx]);
    const auto& sc = scopes_[scope.get()];
    const auto sit = sc.signalIndex.find(first.name);
    if (sit == sc.signalIndex.end())
        return std::nullopt;
    NodeId node = sit->second;
    for (const auto& key : first.indices) {
        const auto it = nodes_[node.get()].memberIndex.find(key);
        if (it == nodes_[node.get()].memberIndex.end())
            return std::nullopt;
        node = it->second;
    }
    ++idx;

    // Фаза 3: оставшиеся сегменты — члены структуры / элементы массива.
    for (; idx < segs.size(); ++idx) {
        const Segment s = parseSegment(segs[idx]);
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

std::optional<ScopeId> Hierarchy::findScope(std::string_view path) const {
    ScopeId scope = kRootId;
    if (path.empty())
        return scope;

    for (const auto seg : splitDots(path)) {
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
