#include "wasafe/model/signal.hpp"

#include "wasafe/core/exception.hpp"
#include "wasafe/model/database.hpp"

namespace WaSafe {

namespace {

const Hierarchy::SignalNode& nodeOf(const Database& db, NodeId i) {
    return db.hierarchy().signalNode(i);
}

}  // namespace

void Signal::throwInvalid() {
    throw Exception{"Signal: access through invalid handle"};
}

std::string_view Signal::name() const {
    require();
    return nodeOf(*db_, node_).name;
}

std::string Signal::fullPath() const {
    require();
    return db_->hierarchy().pathOf(node_);
}

Type Signal::type() const {
    require();
    return nodeOf(*db_, node_).type;
}

TypeKind Signal::kind() const {
    require();
    const auto& t = nodeOf(*db_, node_).type;
    return t ? t->kind() : TypeKind::VOID;
}

std::uint32_t Signal::width() const {
    require();
    const auto& t = nodeOf(*db_, node_).type;
    return t ? t->bitWidth() : 0;
}

bool Signal::isLeaf() const {
    require();
    return nodeOf(*db_, node_).children.empty();
}

bool Signal::isComposite() const {
    require();
    return !isLeaf();
}

bool Signal::hasOwnStream() const {
    require();
    return nodeOf(*db_, node_).stream.valid();
}

std::optional<SignalId> Signal::streamId() const {
    require();
    const auto id = nodeOf(*db_, node_).stream;
    return id.valid() ? std::optional{id} : std::nullopt;
}

std::size_t Signal::childCount() const {
    require();
    return nodeOf(*db_, node_).children.size();
}

Signal Signal::child(std::size_t ordinal) const {
    require();
    const auto& n = nodeOf(*db_, node_);
    if (ordinal >= n.children.size())
        return {};
    return db_->signalHandle(n.children[ordinal]);
}

Signal Signal::child(std::string_view member) const {
    require();
    const auto& n = nodeOf(*db_, node_);
    if (const auto it = n.memberIndex.find(member); it != n.memberIndex.end())
        return db_->signalHandle(it->second);
    return {};
}

Signal Signal::find(std::string_view relative) const {
    require();
    const auto node = db_->hierarchy().findSignal(node_, relative);
    return node.has_value() ? db_->signalHandle(*node) : Signal{};
}

Signal Signal::parent() const {
    require();
    const auto p = nodeOf(*db_, node_).parent;
    return p.valid() ? db_->signalHandle(p) : Signal{};
}

SignalChildRange Signal::children() const {
    require();
    const auto& n = nodeOf(*db_, node_);
    return {db_, n.children.data(), n.children.size()};
}

Value Signal::valueAt(TimeStamp t) const {
    require();
    return db_->valueAt(node_, t);
}

ValueCursor Signal::changes(TimeRange range) const {
    require();
    return db_->changes(node_, range);
}

TimeStamp Signal::nextChange(TimeStamp after) const {
    require();
    return db_->nextChange(node_, after);
}

TimeStamp Signal::prevChange(TimeStamp before) const {
    require();
    return db_->prevChange(node_, before);
}

// --- SignalChildRange::iterator --------------------------------------------
Signal SignalChildRange::Iterator::operator*() const {
    return db_->signalHandle(nodes_[pos_]);
}

}  // namespace WaSafe
