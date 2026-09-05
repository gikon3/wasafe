#include "io/validating_builder.hpp"

#include <format>

#include "wasafe/core/exception.hpp"
#include "wasafe/model/database.hpp"

namespace WaSafe {
namespace {

std::string_view describe(ValueKind k) noexcept {
    switch (k) {
        case ValueKind::LOGIC:
            return "logic";
        case ValueKind::REAL:
            return "real";
        case ValueKind::STRING:
            return "string";
        case ValueKind::AGGREGATE:
            return "aggregate";
        default:
            return "empty";
    }
}

/// Разворачивание один в один повторяет BaseBuilder::buildChildren(): важен не
/// только состав, но и ПОРЯДОК — по нему `leaves` сопоставляется с элементами.
// NOLINTNEXTLINE(misc-no-recursion) — обход композита по определению рекурсивен
void collectExpansion(const Type& type, std::vector<Type>& out) {
    if (!type)
        return;
    if (const auto* st = type->as<StructType>()) {
        if (st->packed())
            return;  // биты поддерева покрыты потоком предка — потоков не выделяется
        for (const StructMember& m : st->members()) {
            if (singleStreamRepresentable(m.type))
                out.push_back(m.type);
            collectExpansion(m.type, out);
        }
    }
    else if (const auto* at = type->as<ArrayType>()) {
        if (at->packed())
            return;
        const Type& elem = at->elementType();
        for (std::size_t ord = 0, count = at->elementCount(); ord < count; ++ord) {
            if (singleStreamRepresentable(elem))
                out.push_back(elem);
            collectExpansion(elem, out);
        }
    }
    // лист — детей нет
}

}  // namespace

std::vector<Type> ValidatingBuilder::expansionTypes(const Type& type) {
    std::vector<Type> out;
    collectExpansion(type, out);
    return out;
}

void ValidatingBuilder::setTimeScale(TimeScale scale) {
    requireHeader("setTimeScale");
    sink_.setTimeScale(scale);
}

ScopeId ValidatingBuilder::beginScope(std::string_view name, ScopeKind kind) {
    requireHeader("beginScope");
    ++openScopes_;
    return sink_.beginScope(name, kind);
}

void ValidatingBuilder::endScope() {
    requireHeader("endScope");
    // Сам приёмник лишний endScope молча игнорирует (BaseBuilder::endScope
    // держит корень на дне стека), поэтому перекос уезжает в иерархию: сигналы
    // после него оказываются не в том scope.
    if (openScopes_ == 0)
        throw Exception{"Builder::endScope: no scope is open"};
    --openScopes_;
    sink_.endScope();
}

SignalId ValidatingBuilder::declareVar(std::string_view name, Type type, std::optional<SignalId> alias,
        std::span<SignalId> leaves) {
    requireHeader("declareVar");
    if (alias)
        requireKnown(*alias, "declareVar", std::format("alias of '{}'", name));

    const std::vector<Type> expansion = expansionTypes(type);
    if (!leaves.empty() && leaves.size() != expansion.size()) {
        // Размер span — правило самого приёмника, и сообщение у него уже
        // внятное; дублировать его тут значило бы разойтись при первой правке.
        return sink_.declareVar(name, type, alias, leaves);
    }

    // Валидный id на входе привязывает элемент к УЖЕ существующему потоку —
    // это тот же алиас, только на уровне элемента, поэтому и проверка та же.
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        if (leaves[i].valid())
            requireKnown(leaves[i], "declareVar", std::format("leaves[{}] of '{}'", i, name));
    }

    // Пустой span и span из невалидных id приёмник обрабатывает одинаково
    // (BaseBuilder::bindStream), но во втором случае выделенные потоки видны
    // здесь. Подставляем свой буфер, чтобы узнать вид и ширину каждого листа.
    std::vector<SignalId> scratch;
    if (leaves.empty() && !expansion.empty()) {
        scratch.resize(expansion.size());
        leaves = scratch;
    }

    const SignalId stream = sink_.declareVar(name, type, alias, leaves);
    if (!alias)
        record(stream, type);
    for (std::size_t i = 0; i < expansion.size(); ++i)
        record(leaves[i], expansion[i]);
    return stream;
}

void ValidatingBuilder::headerDone() {
    if (headerDone_)
        throw Exception{"Builder::headerDone: called twice"};
    if (openScopes_ != 0) {
        throw Exception{std::format("Builder::headerDone: {} scope(s) still open — beginScope/endScope are unbalanced",
                openScopes_)};
    }
    headerDone_ = true;
    sink_.headerDone();
}

void ValidatingBuilder::setTime(TimeStamp time) {
    requireValues("setTime");
    if (timeSet_ && time < now_)
        throw Exception{std::format("Builder::setTime: time went backwards: {} after {}", time, now_)};
    now_ = time;
    timeSet_ = true;
    sink_.setTime(time);
}

void ValidatingBuilder::valueChange(SignalId id, ValueView value) {
    requireValues("valueChange");
    if (!timeSet_)
        throw Exception{"Builder::valueChange: called before the first setTime()"};
    requireKnown(id, "valueChange", "id");

    const Stream& stream = streams_.find(id)->second;
    if (value.kind() != stream.kind) {
        throw Exception{std::format("Builder::valueChange: stream {} was declared {}, but the value is {}", id.get(),
                describe(stream.kind), describe(value.kind()))};
    }
    // Ширину не проверяет никто: длинное значение молча обрезается, а короткое
    // пишет за границу — биты за width() читаются как X, b-бит у X единичный, а
    // план bval двухзначного блока не заведён (см. шапку).
    if (stream.kind == ValueKind::LOGIC && value.logic().width() != stream.width) {
        throw Exception{std::format("Builder::valueChange: stream {} is {} bits wide, but the value is {}", id.get(),
                stream.width, value.logic().width())};
    }
    sink_.valueChange(id, value);
}

void ValidatingBuilder::finish() {
    if (!headerDone_)
        throw Exception{"Builder::finish: called before headerDone()"};
    if (finished_)
        throw Exception{"Builder::finish: called twice"};
    finished_ = true;
    sink_.finish();
}

Database ValidatingBuilder::takeDatabase() {
    if (!finished_)
        throw Exception{"Builder::takeDatabase: called before finish()"};
    if (taken_)
        throw Exception{"Builder::takeDatabase: called twice"};
    taken_ = true;
    return sink_.takeDatabase();
}

void ValidatingBuilder::requireHeader(std::string_view method) const {
    if (headerDone_)
        throw Exception{std::format("Builder::{}: the header phase is over, headerDone() was already called", method)};
}

void ValidatingBuilder::requireValues(std::string_view method) const {
    if (!headerDone_)
        throw Exception{std::format("Builder::{}: the value phase has not started, headerDone() is missing", method)};
    if (finished_)
        throw Exception{std::format("Builder::{}: called after finish()", method)};
}

void ValidatingBuilder::requireKnown(SignalId id, std::string_view method, std::string_view what) const {
    if (!id.valid())
        throw Exception{std::format("Builder::{}: {} is an invalid SignalId", method, what)};
    if (!streams_.contains(id)) {
        throw Exception{
                std::format("Builder::{}: {} refers to stream {}, which was never declared", method, what, id.get())};
    }
}

void ValidatingBuilder::record(SignalId id, const Type& t) {
    if (!id.valid() || !singleStreamRepresentable(t))
        return;
    // Повторная привязка (алиас на уровне элемента) запись НЕ переписывает:
    // поток заведён первым объявлением, и в storage лежат именно его вид и
    // ширина. Ядро расхождение не проверяет — см. Builder::declareVar.
    if (streams_.contains(id))
        return;

    switch (t->kind()) {
        case TypeKind::REAL:
            streams_.emplace(id, Stream{ValueKind::REAL, 0});
            break;
        case TypeKind::STRING:
            streams_.emplace(id, Stream{ValueKind::STRING, 0});
            break;
        default:  // scalar/vector/enum и packed-композит — один logic-вектор
            streams_.emplace(id, Stream{ValueKind::LOGIC, t->bitWidth()});
            break;
    }
}

}  // namespace WaSafe
