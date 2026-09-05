#include "wasafe/model/database.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "storage/merge_cursor.hpp"

namespace WaSafe {

namespace {

/// Переходник над мультипотоковым курсором: переписывает номер источника по
/// таблице. Нужен, когда в запрос попали и листья со своим потоком (их курсор
/// нумерует по своему, более короткому списку), и узлы, требующие отдельного
/// подкурсора, — снаружи номера обязаны быть индексами в запрошенном span.
class RemapCursor final : public Cursor {
public:
    RemapCursor(std::unique_ptr<Cursor> src, std::vector<std::uint32_t> map) :
            src_{std::move(src)}, map_{std::move(map)} {}

    [[nodiscard]] bool next() override {
        if (!src_->next())
            return false;
        current_ = src_->current();
        current_.source = map_[current_.source];
        return true;
    }

    [[nodiscard]] const ValueChange& current() const noexcept override { return current_; }

private:
    std::unique_ptr<Cursor> src_;
    std::vector<std::uint32_t> map_;
    ValueChange current_{};
};

/// Вырезать [offset, offset+width) из logic-значения предка. Выход за ширину
/// источника не проверяется намеренно: на таких битах верный ответ — Logic::X
/// («биты не записаны»).
LogicVector extractSlice(LogicVectorView src, BitSlice slice) {
    return LogicVector::fromSlice(src, slice.offset, slice.width);
}

/// Курсор поверх курсора потока-предка: вырезает BitSlice и отбрасывает записи,
/// где срез не изменился (соседние биты того же вектора менялись, а этот член —
/// нет). Стартовое prev_ — значение среза, ПЕРЕНЕСЁННОЕ в диапазон снаружи;
/// nullopt означает «до диапазона значения не было», и тогда первая запись
/// отдаётся безусловно. Благодаря переносу результат не зависит от границ окна.
class SliceCursor final : public Cursor {
public:
    SliceCursor(std::unique_ptr<Cursor> src, BitSlice slice, std::optional<LogicVector> carryIn) :
            src_{std::move(src)}, slice_{slice}, prev_{std::move(carryIn)} {}

    [[nodiscard]] bool next() override {
        while (src_->next()) {
            const ValueChange& change = src_->current();
            if (change.value.kind() != ValueKind::LOGIC)
                continue;

            LogicVector cut = extractSlice(change.value.logic(), slice_);
            if (prev_ && *prev_ == cut)
                continue;

            prev_ = std::move(cut);
            current_.time = change.time;
            current_.value = *prev_;  // вид на собственный буфер — валиден до next()
            return true;
        }
        return false;
    }

    [[nodiscard]] const ValueChange& current() const noexcept override { return current_; }

private:
    std::unique_ptr<Cursor> src_;
    BitSlice slice_;
    std::optional<LogicVector> prev_;
    ValueChange current_{};
};

/// Скопировать невладеющий вид листового значения в самостоятельный Value
/// (снимок, переживающий следующее обращение к storage).
Value materialize(ValueView v) {
    switch (v.kind()) {
        case ValueKind::LOGIC:
            return Value{v.logic()};  // ctor копирует оба бит-плана из вида
        case ValueKind::REAL:
            return Value{v.real()};
        case ValueKind::STRING:
            return Value{std::string{v.string()}};
        default:
            return {};
    }
}

}  // namespace

Database::Database(Hierarchy hierarchy, std::unique_ptr<Storage> storage) :
        Database{std::make_shared<const Hierarchy>(std::move(hierarchy)), std::move(storage)} {
}

Database::Database(std::shared_ptr<const Hierarchy> hierarchy, std::unique_ptr<Storage> storage) :
        hierarchy_{std::move(hierarchy)}, storage_{std::move(storage)} {
}

void Database::throwMovedFrom() {
    throw Exception{"database: обращение к перемещённой Database"};
}

Database Database::duplicate() const {
    auto storage = storage_->duplicate();
    if (!storage)
        throw Exception{"database: storage does not support duplication"};
    return Database{hierarchy_, std::move(storage)};
}

Scope Database::root() const {
    return scopeHandle(hierarchy().root());
}

std::optional<Signal> Database::find(std::string_view path) const {
    const auto node = hierarchy().findSignal(path);
    return node ? std::optional{signalHandle(*node)} : std::nullopt;
}

std::optional<Scope> Database::findScope(std::string_view path) const {
    const auto id = hierarchy().findScope(path);
    return id ? std::optional{scopeHandle(*id)} : std::nullopt;
}

// NOLINTNEXTLINE(misc-no-recursion) — обход дерева композита по определению рекурсивен
Value Database::valueAt(NodeId id, TimeStamp timestamp) const {
    const auto& node = hierarchy().signalNode(id);

    // Композит: значение собирается из детей независимо от того, есть ли у узла
    // собственный поток (packed-структура хранится одним вектором, но наружу
    // отдаётся как агрегат её членов).
    if (!node.children.empty()) {
        std::vector<Value> parts;
        parts.reserve(node.children.size());
        for (const auto child : node.children)
            parts.push_back(valueAt(child, timestamp));
        return Value{Aggregate{std::move(parts)}};
    }

    // Packed-член: вырезать [offset, offset+width) из ближайшего предка,
    // владеющего потоком (смещение абсолютно в пределах этого потока).
    if (node.projection) {
        const NodeId anc = owningAncestor(id);
        if (!anc.valid())
            return {};
        auto cut = sliceAt(hierarchy().signalNode(anc).stream, *node.projection, timestamp);
        return cut ? Value{std::move(*cut)} : Value{};
    }

    // Лист со своим потоком.
    if (node.stream.valid())
        return materialize(storage_->valueAt(node.stream, timestamp));

    return {};
}

std::unique_ptr<Cursor> Database::openNodeCursor(NodeId id, TimeRange range) const {
    // Packed-член: изменения — это моменты, когда менялся ЕГО срез потока предка,
    // а не все изменения этого потока. Композит (даже packed) идёт общим путём
    // через детей — как и valueAt, отдающий для него агрегат.
    if (const auto& node = hierarchy().signalNode(id);
            node.projection && node.children.empty() && !node.stream.valid()) {
        const NodeId anc = owningAncestor(id);
        if (!anc.valid())
            return nullptr;

        const SignalId stream = hierarchy().signalNode(anc).stream;
        const BitSlice slice = *node.projection;
        // Значение, перенесённое в окно: последнее изменение предка ДО range.begin.
        std::optional<LogicVector> carryIn;
        if (const TimeStamp t = storage_->prevChange(stream, range.begin); t != kNoTime)
            carryIn = sliceAt(stream, slice, t);
        return std::make_unique<SliceCursor>(storage_->openCursor(stream, range), slice, std::move(carryIn));
    }

    // Композит без собственного потока сливает листовые потоки поддерева.
    std::vector<SignalId> streams;
    collectLeafStreams(id, streams);
    if (streams.empty())
        return nullptr;
    if (streams.size() == 1)
        return storage_->openCursor(streams[0], range);
    // source здесь — индекс листа в leafNodes(id): у листьев тот же порядок
    // обхода в глубину, что и у collectLeafStreams.
    return storage_->openCursor(streams, range);
}

ValueCursor Database::changes(NodeId id, TimeRange range) const {
    auto cur = openNodeCursor(id, range);
    return cur ? ValueCursor{std::move(cur)} : ValueCursor{};
}

ValueCursor Database::changes(std::span<const NodeId> nodes, TimeRange range) const {
    // Узлы делятся на два класса. «Простой» — это узел с собственным потоком (в
    // том числе packed-структура целиком): его обслуживает прямой SignalId, и
    // все такие узлы уходят в storage ОДНИМ пакетным вызовом — ради него всё и
    // затевалось. «Сложный» — packed-член (нужен SliceCursor) или композит без
    // своего потока (нужно внутреннее слияние листьев): для него подкурсор
    // строится отдельно.
    std::vector<SignalId> streams;         // потоки простых узлов
    std::vector<std::uint32_t> streamMap;  // индекс потока -> индекс узла в nodes
    std::vector<std::unique_ptr<Cursor>> subs;
    std::vector<std::uint32_t> sources;

    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        const NodeId id = nodes[i];
        if (const SignalId stream = hierarchy().signalNode(id).stream; stream.valid()) {
            streams.push_back(stream);
            streamMap.push_back(i);
            continue;
        }
        if (auto cur = openNodeCursor(id, range)) {
            subs.push_back(std::move(cur));
            sources.push_back(i);
        }  // узел без значений (пустой композит) в слиянии не участвует
    }

    // Все узлы простые — пакетный курсор отдаётся как есть: его нумерация уже
    // совпадает с nodes, обёртки не нужны.
    if (subs.empty() && streams.size() == nodes.size())
        return ValueCursor{storage_->openCursor(streams, range)};

    if (!streams.empty()) {
        subs.push_back(std::make_unique<RemapCursor>(storage_->openCursor(streams, range), std::move(streamMap)));
        sources.push_back(MergeCursor::kKeepSource);
    }
    if (subs.empty())
        return {};
    return ValueCursor{std::make_unique<MergeCursor>(std::move(subs), std::move(sources))};
}

// NOLINTNEXTLINE(misc-no-recursion) — обход дерева композита по определению рекурсивен
TimeStamp Database::nextChange(NodeId id, TimeStamp after) const {
    const auto& node = hierarchy().signalNode(id);
    if (node.stream.valid())
        return storage_->nextChange(node.stream, after);

    // Packed-член: идти по фронтам предка, пока срез не изменится. Сравнение
    // optional'ов заодно ловит «значения ещё не было» — появление среза это тоже
    // изменение.
    if (node.projection && node.children.empty()) {
        const NodeId anc = owningAncestor(id);
        if (!anc.valid())
            return kNoTime;

        const SignalId stream = hierarchy().signalNode(anc).stream;
        const BitSlice slice = *node.projection;
        std::optional<LogicVector> prev = sliceAt(stream, slice, after);
        for (TimeStamp t = storage_->nextChange(stream, after); t != kNoTime; t = storage_->nextChange(stream, t)) {
            std::optional<LogicVector> cut = sliceAt(stream, slice, t);
            if (prev != cut)
                return t;
            prev = std::move(cut);
        }
        return kNoTime;
    }

    // Композит: ближайший фронт — минимум среди детей.
    TimeStamp best = kNoTime;
    for (const auto child : node.children) {
        const TimeStamp t = nextChange(child, after);
        if (t != kNoTime && (best == kNoTime || t < best))
            best = t;
    }
    return best;
}

// NOLINTNEXTLINE(misc-no-recursion) — обход дерева композита по определению рекурсивен
TimeStamp Database::prevChange(NodeId id, TimeStamp before) const {
    const auto& node = hierarchy().signalNode(id);
    if (node.stream.valid())
        return storage_->prevChange(node.stream, before);

    // Packed-член: зеркально nextChange. Срез предыдущего фронта переносится в
    // следующую итерацию, поэтому на шаг приходится одно чтение значения.
    if (node.projection && node.children.empty()) {
        const NodeId anc = owningAncestor(id);
        if (!anc.valid())
            return kNoTime;

        const SignalId stream = hierarchy().signalNode(anc).stream;
        const BitSlice slice = *node.projection;
        TimeStamp t = storage_->prevChange(stream, before);
        std::optional<LogicVector> cur = t == kNoTime ? std::nullopt : sliceAt(stream, slice, t);
        while (t != kNoTime) {
            const TimeStamp p = storage_->prevChange(stream, t);
            if (p == kNoTime)
                return cur ? t : kNoTime;  // первое изменение потока: срез появился

            std::optional<LogicVector> prev = sliceAt(stream, slice, p);
            if (prev != cur)
                return t;
            t = p;
            cur = std::move(prev);
        }
        return kNoTime;
    }

    // Композит: ближайший предыдущий фронт — максимум среди детей.
    TimeStamp best = kNoTime;
    for (const auto child : node.children) {
        const TimeStamp t = prevChange(child, before);
        if (t != kNoTime && (best == kNoTime || t > best))
            best = t;
    }
    return best;
}

Signal Database::signalHandle(NodeId id) const {
    return Signal{this, id};
}

Scope Database::scopeHandle(ScopeId id) const {
    return Scope{this, id};
}

NodeId Database::owningAncestor(NodeId id) const {
    NodeId anc = hierarchy().signalNode(id).parent;
    while (anc.valid() && !hierarchy().signalNode(anc).stream.valid())
        anc = hierarchy().signalNode(anc).parent;
    return anc;
}

std::optional<LogicVector> Database::sliceAt(SignalId stream, BitSlice slice, TimeStamp t) const {
    // Результат владеющий: вид из storage живёт лишь до следующего обращения к
    // нему (у ленивого — до вытеснения блока из кэша), а вызывающие сравнивают
    // срезы двух разных моментов.
    const ValueView v = storage_->valueAt(stream, t);
    if (v.kind() != ValueKind::LOGIC)
        return std::nullopt;
    return extractSlice(v.logic(), slice);
}

// NOLINTNEXTLINE(misc-no-recursion) — обход дерева композита по определению рекурсивен
void Database::collectLeafStreams(NodeId id, std::vector<SignalId>& out) const {
    const auto& node = hierarchy().signalNode(id);
    if (node.stream.valid()) {
        out.push_back(node.stream);
        return;
    }  // поток покрывает поддерево

    for (const auto child : node.children)
        collectLeafStreams(child, out);
}

// NOLINTNEXTLINE(misc-no-recursion) — обход дерева композита по определению рекурсивен
void Database::collectLeafNodes(NodeId id, std::vector<NodeId>& out) const {
    // Точное зеркало collectLeafStreams — и порядок, и отбор: на этом держится
    // обещание changes(NodeId) о том, что source — индекс в leafNodes(id).
    // Поэтому лист без собственного потока сюда не попадает: он не даёт потока,
    // а значит и ни одного изменения в слиянии.
    const auto& node = hierarchy().signalNode(id);
    if (node.stream.valid()) {
        out.push_back(id);
        return;
    }  // поток покрывает поддерево

    for (const auto child : node.children)
        collectLeafNodes(child, out);
}

// NOLINTNEXTLINE(misc-no-recursion) — обход дерева scope по определению рекурсивен
void Database::collectLeafNodes(ScopeId scope, std::vector<NodeId>& out) const {
    const auto& node = hierarchy().scopeNode(scope);
    for (const auto sig : node.signals)
        collectLeafNodes(sig, out);
    for (const auto child : node.childScopes)
        collectLeafNodes(child, out);
}

std::vector<NodeId> Database::leafNodes(NodeId id) const {
    std::vector<NodeId> out;
    collectLeafNodes(id, out);
    return out;
}

std::vector<NodeId> Database::leafNodes(ScopeId scope) const {
    std::vector<NodeId> out;
    collectLeafNodes(scope, out);
    return out;
}

}  // namespace WaSafe
