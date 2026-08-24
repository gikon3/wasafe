#include "wasafe/model/signal.hpp"

#include "wasafe/storage/database.hpp"

namespace WaSafe {

namespace {

const Hierarchy::SignalNode& nodeOf(const Database& db, NodeId i) {
    return db.hierarchy().signalNode(i);
}

}  // namespace

std::string_view Signal::name() const {
    return nodeOf(*db_, node_).name;
}

std::string Signal::fullPath() const {
    return db_->hierarchy().pathOf(node_);
}

Type Signal::type() const {
    return nodeOf(*db_, node_).type;
}

TypeKind Signal::kind() const {
    const auto& t = nodeOf(*db_, node_).type;
    return t ? t->kind() : TypeKind::VOID;
}

std::uint32_t Signal::width() const {
    const auto& t = nodeOf(*db_, node_).type;
    return t ? t->bitWidth() : 0;
}

bool Signal::isLeaf() const {
    return nodeOf(*db_, node_).children.empty();
}

bool Signal::isComposite() const {
    return !isLeaf();
}

bool Signal::hasOwnStream() const {
    return nodeOf(*db_, node_).stream.valid();
}

std::optional<SignalId> Signal::streamId() const {
    const auto id = nodeOf(*db_, node_).stream;
    return id.valid() ? std::optional{id} : std::nullopt;
}

std::size_t Signal::childCount() const {
    return nodeOf(*db_, node_).children.size();
}

Signal Signal::child(std::size_t ordinal) const {
    const auto& n = nodeOf(*db_, node_);
    if (ordinal >= n.children.size())
        return {};
    return db_->signalHandle(n.children[ordinal]);
}

Signal Signal::child(std::string_view member) const {
    const auto& n = nodeOf(*db_, node_);
    if (const auto it = n.memberIndex.find(member); it != n.memberIndex.end())
        return db_->signalHandle(it->second);
    return {};
}

Signal Signal::parent() const {
    const auto p = nodeOf(*db_, node_).parent;
    return p.valid() ? db_->signalHandle(p) : Signal{};
}

SignalChildRange Signal::children() const {
    const auto& n = nodeOf(*db_, node_);
    return {db_, n.children.data(), n.children.size()};
}

Value Signal::valueAt(TimeStamp t) const {
    return db_->valueAt(node_, t);
}

ValueCursor Signal::changes(TimeRange range) const {
    return db_->changes(node_, range);
}

TimeStamp Signal::nextChange(TimeStamp after) const {
    return db_->nextChange(node_, after);
}

TimeStamp Signal::prevChange(TimeStamp before) const {
    return db_->prevChange(node_, before);
}

// --- SignalChildRange::iterator --------------------------------------------
Signal SignalChildRange::Iterator::operator*() const {
    return db_->signalHandle(nodes_[pos_]);
}

}  // namespace WaSafe
