#include "wasafe/model/scope.hpp"

#include "wasafe/core/exception.hpp"
#include "wasafe/storage/database.hpp"

namespace WaSafe {

namespace {

const Hierarchy::ScopeNode& nodeOf(const Database& db, ScopeId id) {
    return db.hierarchy().scopeNode(id);
}

}  // namespace

std::string_view toString(ScopeKind k) noexcept {
    switch (k) {
        case ScopeKind::ROOT:
            return "root";
        case ScopeKind::MODULE:
            return "module";
        case ScopeKind::INTERFACE:
            return "interface";
        case ScopeKind::PACKAGE:
            return "package";
        case ScopeKind::PROGRAM:
            return "program";
        case ScopeKind::TASK:
            return "task";
        case ScopeKind::FUNCTION:
            return "function";
        case ScopeKind::BLOCK:
            return "block";
        case ScopeKind::GENERATE_BLOCK:
            return "generate";
        case ScopeKind::STRUCT:
            return "struct";
        case ScopeKind::UNION:
            return "union";
        case ScopeKind::ARRAY:
            return "array";
        case ScopeKind::CLASS:
            return "class";
        case ScopeKind::UNKNOWN:
            return "unknown";
    }
    return "unknown";
}

void Scope::throwInvalid() {
    throw Exception{"Scope: access through invalid handle"};
}

std::string_view Scope::name() const {
    require();
    return nodeOf(*db_, id_).name;
}

std::string Scope::fullPath() const {
    require();
    return db_->hierarchy().pathOf(id_);
}

ScopeKind Scope::kind() const {
    require();
    return nodeOf(*db_, id_).kind;
}

Scope Scope::parent() const {
    require();
    const auto p = nodeOf(*db_, id_).parent;
    return p.valid() ? db_->scopeHandle(p) : Scope{};
}

std::size_t Scope::scopeCount() const {
    require();
    return nodeOf(*db_, id_).childScopes.size();
}

Scope Scope::scope(std::size_t index) const {
    require();
    const auto& n = nodeOf(*db_, id_);
    return index < n.childScopes.size() ? db_->scopeHandle(n.childScopes[index]) : Scope{};
}

Scope Scope::scope(std::string_view name) const {
    require();
    const auto& n = nodeOf(*db_, id_);
    if (const auto it = n.scopeIndex.find(name); it != n.scopeIndex.end()) {
        return db_->scopeHandle(it->second);
    }
    return {};
}

ScopeRange Scope::scopes() const {
    require();
    return {db_, id_, scopeCount()};
}

std::size_t Scope::signalCount() const {
    require();
    return nodeOf(*db_, id_).signals.size();
}

Signal Scope::signal(std::size_t index) const {
    require();
    const auto& n = nodeOf(*db_, id_);
    return index < n.signals.size() ? db_->signalHandle(n.signals[index]) : Signal{};
}

Signal Scope::signal(std::string_view name) const {
    require();
    const auto& n = nodeOf(*db_, id_);
    if (const auto it = n.signalIndex.find(name); it != n.signalIndex.end())
        return db_->signalHandle(it->second);
    return {};
}

SignalChildRange Scope::signals() const {
    require();
    const auto& n = nodeOf(*db_, id_);
    return {db_, n.signals.data(), n.signals.size()};
}

Signal Scope::find(std::string_view relative) const {
    require();
    const auto node = db_->hierarchy().findSignal(id_, relative);
    return node.has_value() ? db_->signalHandle(*node) : Signal{};
}

Scope Scope::findScope(std::string_view relative) const {
    require();
    const auto scope = db_->hierarchy().findScope(id_, relative);
    return scope.has_value() ? db_->scopeHandle(*scope) : Scope{};
}

// --- ScopeRange::iterator ---------------------------------------------------
Scope ScopeRange::Iterator::operator*() const {
    const auto& n = db_->hierarchy().scopeNode(parent_);
    return db_->scopeHandle(n.childScopes[pos_]);
}

}  // namespace WaSafe
