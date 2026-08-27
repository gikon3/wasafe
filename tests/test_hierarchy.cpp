#include <gtest/gtest.h>

#include <optional>
#include <tuple>
#include <vector>

#include "wasafe/core/exception.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/model/hierarchy.hpp"
#include "wasafe/storage/database.hpp"

using namespace WaSafe;

namespace {

/// Иерархия для относительного поиска: top.clk и top.cpu с двумя композитами —
/// regs[0:1].{addr,valid} и двумерным mem[0:1][0:1].
Hierarchy buildNested() {
    Hierarchy h;
    const ScopeId top = h.addScope(h.root(), "top", ScopeKind::MODULE);
    const ScopeId cpu = h.addScope(top, "cpu", ScopeKind::MODULE);
    h.addSignal(top, "clk", makeScalar(), SignalId{0});

    const NodeId regs = h.addSignal(cpu, "regs", makeScalar(), SignalId{});
    for (std::int32_t i = 0; i < 2; ++i) {
        const NodeId elem = h.addElement(regs, i, makeScalar(), SignalId{});
        h.addMember(elem, "addr", makeVector(7, 0), SignalId{static_cast<SignalId::ValueType>(2 * i + 1)});
        h.addMember(elem, "valid", makeScalar(), SignalId{static_cast<SignalId::ValueType>(2 * i + 2)});
    }

    const NodeId mem = h.addSignal(cpu, "mem", makeScalar(), SignalId{});
    for (std::int32_t i = 0; i < 2; ++i) {
        const NodeId row = h.addElement(mem, i, makeScalar(), SignalId{});
        for (std::int32_t j = 0; j < 2; ++j)
            h.addElement(row, j, makeScalar(), SignalId{});
    }
    return h;
}

/// Та же форма, но собранная штатным Builder'ом — нужна для хэндлов
/// Signal/Scope, которым требуется Database.
Database buildNestedDb() {
    auto sink = makeMemoryBuilder();
    sink->setTimeScale({.exponent = -9, .scale = 1});

    sink->beginScope("top", ScopeKind::MODULE);
    std::ignore = sink->declareVar("clk", makeScalar());

    sink->beginScope("cpu", ScopeKind::MODULE);
    const Type cellT = makeStruct({{"addr", makeVector(7, 0), 0}, {"valid", makeScalar(), 8}}, /*packed*/ true);
    const Type regsT = makeArray(cellT, 0, 1, /*packed*/ false);
    std::vector<SignalId> leaves(expansionStreamCount(regsT));
    std::ignore = sink->declareVar("regs", regsT, std::nullopt, leaves);
    sink->endScope();

    sink->endScope();
    sink->headerDone();
    sink->finish();
    return sink->takeDatabase();
}

}  // namespace

// построение иерархии: scope и сигналы
TEST(Hierarchy, BuildScopesAndSignals) {
    Hierarchy h;
    const ScopeId top = h.addScope(h.root(), "top", ScopeKind::MODULE);
    const ScopeId cpu = h.addScope(top, "cpu", ScopeKind::MODULE);

    const NodeId clk = h.addSignal(top, "clk", makeScalar(), SignalId{0});
    const NodeId pc = h.addSignal(cpu, "pc", makeVector(31, 0), SignalId{1});

    EXPECT_EQ(h.scopeNode(top).name, "top");
    EXPECT_EQ(h.scopeNode(top).childScopes.size(), 1u);
    EXPECT_EQ(h.scopeNode(cpu).parent, top);

    EXPECT_EQ(h.signalNode(clk).name, "clk");
    EXPECT_EQ(h.signalNode(clk).type->bitWidth(), 1u);
    EXPECT_EQ(h.signalNode(pc).type->bitWidth(), 32u);
    EXPECT_EQ(h.scopeNode(cpu).signals.size(), 1u);
}

// композит: члены структуры как дочерние узлы
TEST(Hierarchy, CompositeStructMembers) {
    Hierarchy h;
    const ScopeId top = h.addScope(h.root(), "top", ScopeKind::MODULE);

    // packed-структура хранится одним потоком; члены — битовые срезы.
    const NodeId req = h.addSignal(top, "req", makeScalar(), SignalId{0});
    h.addMember(req, "addr", makeVector(7, 0), SignalId{}, BitSlice{.offset = 1, .width = 8});
    h.addMember(req, "valid", makeScalar(), SignalId{}, BitSlice{.offset = 0, .width = 1});

    const auto& node = h.signalNode(req);
    ASSERT_EQ(node.children.size(), 2u);
    EXPECT_EQ(node.memberIndex.at("valid"), node.children[1]);
    ASSERT_TRUE(h.signalNode(node.children[0]).projection.has_value());
    EXPECT_EQ(h.signalNode(node.children[0]).projection->width, 8u);
}

// find_signal: спуск по scope и поиск сигнала
TEST(Hierarchy, FindSignalDescent) {
    Hierarchy h;
    const ScopeId top = h.addScope(h.root(), "top", ScopeKind::MODULE);
    const ScopeId cpu = h.addScope(top, "cpu", ScopeKind::MODULE);
    const NodeId clk = h.addSignal(top, "clk", makeScalar(), SignalId{0});
    const NodeId pc = h.addSignal(cpu, "pc", makeVector(31, 0), SignalId{1});

    EXPECT_EQ(h.findSignal("top.clk"), clk);
    EXPECT_EQ(h.findSignal("top.cpu.pc"), pc);

    // Несуществующее и «путь к scope, а не к сигналу» -> nullopt.
    EXPECT_FALSE(h.findSignal("top.nope").has_value());
    EXPECT_FALSE(h.findSignal("top.cpu").has_value());
    EXPECT_FALSE(h.findSignal("").has_value());
}

// find_signal: члены структуры и элементы массива
TEST(Hierarchy, FindSignalMembersAndElements) {
    Hierarchy h;
    const ScopeId top = h.addScope(h.root(), "top", ScopeKind::MODULE);

    // regs[0:1] из структур {addr[7:0], valid}.
    const NodeId regs = h.addSignal(top, "regs", makeScalar(), SignalId{});
    for (std::int32_t i = 0; i < 2; ++i) {
        const NodeId elem = h.addElement(regs, i, makeScalar(), SignalId{});
        h.addMember(elem, "addr", makeVector(7, 0), SignalId{static_cast<SignalId::ValueType>(2 * i)});
        h.addMember(elem, "valid", makeScalar(), SignalId{static_cast<SignalId::ValueType>(2 * i + 1)});
    }

    const auto v1 = h.findSignal("top.regs[1].valid");
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(h.signalNode(*v1).name, "valid");

    // Точка перед скобкой эквивалентна склеенной записи.
    EXPECT_EQ(h.findSignal("top.regs.[0].addr"), h.findSignal("top.regs[0].addr"));

    EXPECT_FALSE(h.findSignal("top.regs[5].valid").has_value());  // нет такого элемента
    EXPECT_FALSE(h.findSignal("top.regs[0].nope").has_value());   // нет такого члена
}

// find_scope и path_of
TEST(Hierarchy, FindScopeAndPathOf) {
    Hierarchy h;
    const ScopeId top = h.addScope(h.root(), "top", ScopeKind::MODULE);
    const ScopeId cpu = h.addScope(top, "cpu", ScopeKind::MODULE);
    const NodeId regs = h.addSignal(cpu, "regs", makeScalar(), SignalId{});
    const NodeId elem = h.addElement(regs, 3, makeScalar(), SignalId{});
    const NodeId fld = h.addMember(elem, "valid", makeScalar(), SignalId{0});

    EXPECT_EQ(h.findScope("top.cpu"), cpu);
    EXPECT_EQ(h.findScope(""), h.root());
    EXPECT_FALSE(h.findScope("top.nope").has_value());

    EXPECT_EQ(h.pathOf(cpu), "top.cpu");
    EXPECT_EQ(h.pathOf(regs), "top.cpu.regs");
    EXPECT_EQ(h.pathOf(elem), "top.cpu.regs[3]");
    EXPECT_EQ(h.pathOf(fld), "top.cpu.regs[3].valid");

    // path_of и find_signal — взаимно обратны.
    EXPECT_EQ(h.findSignal(h.pathOf(fld)), fld);
}

// path_of: пустое имя промежуточного scope сохраняется как пустой сегмент
// (двойная точка), но пустое имя корня не даёт ведущей точки.
TEST(Hierarchy, PathOfKeepsEmptyMiddleScope) {
    Hierarchy h;
    const ScopeId top = h.addScope(h.root(), "top", ScopeKind::MODULE);
    const ScopeId anon = h.addScope(top, "", ScopeKind::MODULE);  // безымянный scope в середине
    const ScopeId cpu = h.addScope(anon, "cpu", ScopeKind::MODULE);
    const NodeId clk = h.addSignal(cpu, "clk", makeScalar(), SignalId{0});

    EXPECT_EQ(h.pathOf(top), "top");       // корень не добавляет ведущую точку: не ".top"
    EXPECT_EQ(h.pathOf(anon), "top.");     // пустой сегмент сохраняется
    EXPECT_EQ(h.pathOf(cpu), "top..cpu");  // структура цела: двойная точка остаётся
    EXPECT_EQ(h.pathOf(clk), "top..cpu.clk");

    // find* согласованы с pathOf: пустой сегмент резолвится в безымянный scope,
    // поэтому обратимость сохраняется и при двойных/хвостовых точках.
    EXPECT_EQ(h.findScope("top."), anon);
    EXPECT_EQ(h.findScope("top..cpu"), cpu);
    EXPECT_EQ(h.findScope(h.pathOf(cpu)), cpu);
    EXPECT_EQ(h.findSignal("top..cpu.clk"), clk);
    EXPECT_EQ(h.findSignal(h.pathOf(clk)), clk);
}

// Невалидные хэндлы ScopeId/NodeId отвергаются исключением, а не молча
// (пустой строкой) или через UB при индексировании.
TEST(Hierarchy, InvalidHandlesThrow) {
    Hierarchy h;
    const ScopeId top = h.addScope(h.root(), "top", ScopeKind::MODULE);
    const NodeId sig = h.addSignal(top, "sig", makeScalar(), SignalId{0});

    const ScopeId badScope{12345};  // вне диапазона
    const ScopeId nullScope{};      // невалидный сентинел
    const NodeId badNode{999};

    // доступ
    EXPECT_THROW((void)h.scopeNode(badScope), Exception);
    EXPECT_THROW((void)h.scopeNode(nullScope), Exception);
    EXPECT_THROW((void)h.signalNode(badNode), Exception);
    // pathOf — раньше молча возвращал ""
    EXPECT_THROW((void)h.pathOf(badScope), Exception);
    EXPECT_THROW((void)h.pathOf(nullScope), Exception);
    EXPECT_THROW((void)h.pathOf(badNode), Exception);
    // относительный поиск — стартовый хэндл проверяется так же
    EXPECT_THROW((void)h.findSignal(badNode, "x"), Exception);
    EXPECT_THROW((void)h.findSignal(badScope, "x"), Exception);
    EXPECT_THROW((void)h.findScope(badScope, "x"), Exception);
    EXPECT_THROW((void)h.findScope(nullScope, "x"), Exception);
    // построение — раньше воспринимало родителя на веру (UB)
    EXPECT_THROW((void)h.addScope(badScope, "x", ScopeKind::MODULE), Exception);
    EXPECT_THROW((void)h.addSignal(badScope, "x", makeScalar(), SignalId{}), Exception);
    EXPECT_THROW((void)h.addMember(badNode, "x", makeScalar(), SignalId{}), Exception);
    EXPECT_THROW((void)h.addElement(badNode, 0, makeScalar(), SignalId{}), Exception);

    // валидные хэндлы по-прежнему работают
    EXPECT_NO_THROW((void)h.scopeNode(top));
    EXPECT_NO_THROW((void)h.signalNode(sig));
    EXPECT_EQ(h.pathOf(sig), "top.sig");
}

// Относительный поиск от узла-сигнала: путь из членов и элементов.
TEST(Hierarchy, FindSignalRelativeToNode) {
    const Hierarchy h = buildNested();
    const auto regs = h.findSignal("top.cpu.regs");
    ASSERT_TRUE(regs.has_value());

    EXPECT_EQ(h.findSignal(*regs, "[1].valid"), h.findSignal("top.cpu.regs[1].valid"));
    EXPECT_EQ(h.findSignal(*regs, "[0]"), h.findSignal("top.cpu.regs[0]"));

    // Грамматика та же, что у абсолютного пути: пробелы внутри скобок
    // нормализуются, несколько индексов в одном сегменте разбираются подряд.
    EXPECT_EQ(h.findSignal(*regs, "[ 1 ].valid"), h.findSignal("top.cpu.regs[1].valid"));
    const auto mem = h.findSignal("top.cpu.mem");
    ASSERT_TRUE(mem.has_value());
    EXPECT_EQ(h.findSignal(*mem, "[1][0]"), h.findSignal("top.cpu.mem[1][0]"));

    // Пустой путь именует сам узел — он и есть сигнал.
    EXPECT_EQ(h.findSignal(*regs, ""), regs);

    EXPECT_FALSE(h.findSignal(*regs, "[5]").has_value());       // нет такого элемента
    EXPECT_FALSE(h.findSignal(*regs, "[0].nope").has_value());  // нет такого члена
    EXPECT_FALSE(h.findSignal(*regs, "cpu").has_value());       // спуска по scope от сигнала нет
}

// Относительный поиск от scope: вложенные scope, затем сигнал.
TEST(Hierarchy, FindSignalRelativeToScope) {
    const Hierarchy h = buildNested();
    const auto top = h.findScope("top");
    ASSERT_TRUE(top.has_value());

    EXPECT_EQ(h.findSignal(*top, "clk"), h.findSignal("top.clk"));
    EXPECT_EQ(h.findSignal(*top, "cpu.regs[1].addr"), h.findSignal("top.cpu.regs[1].addr"));

    // Пустой путь именует scope, а не сигнал.
    EXPECT_FALSE(h.findSignal(*top, "").has_value());
    EXPECT_FALSE(h.findSignal(*top, "nope").has_value());
    EXPECT_FALSE(h.findSignal(*top, "cpu").has_value());  // это scope, а не сигнал

    // Абсолютная перегрузка — частный случай поиска от корня.
    EXPECT_EQ(h.findSignal(h.root(), "top.cpu.regs[0].valid"), h.findSignal("top.cpu.regs[0].valid"));
}

// Относительный поиск scope от scope.
TEST(Hierarchy, FindScopeRelativeToScope) {
    const Hierarchy h = buildNested();
    const auto top = h.findScope("top");
    ASSERT_TRUE(top.has_value());

    EXPECT_EQ(h.findScope(*top, "cpu"), h.findScope("top.cpu"));
    EXPECT_EQ(h.findScope(*top, ""), top);  // пустой путь — сам scope
    EXPECT_FALSE(h.findScope(*top, "nope").has_value());
    EXPECT_EQ(h.findScope(h.root(), "top.cpu"), h.findScope("top.cpu"));
}

// Хэндлы: Signal::find, Scope::find, Scope::findScope.
TEST(Hierarchy, HandleRelativeLookup) {
    const Database db = buildNestedDb();

    const auto regs = db.find("top.cpu.regs");
    ASSERT_TRUE(regs.has_value());

    const Signal valid = regs->find("[1].valid");
    ASSERT_TRUE(valid.valid());
    EXPECT_EQ(valid.fullPath(), "top.cpu.regs[1].valid");

    // Эквивалентно абсолютному поиску по собранному пути, но без сборки строки.
    const auto absolute = db.find("top.cpu.regs[1].valid");
    ASSERT_TRUE(absolute.has_value());
    EXPECT_EQ(valid.node(), absolute->node());
    EXPECT_EQ(valid, *absolute);

    EXPECT_FALSE(regs->find("[5]").valid());
    EXPECT_EQ(regs->find("").node(), regs->node());  // пустой путь — сам сигнал

    const auto top = db.findScope("top");
    ASSERT_TRUE(top.has_value());
    EXPECT_EQ(top->find("cpu.regs[0].addr").fullPath(), "top.cpu.regs[0].addr");
    EXPECT_FALSE(top->find("").valid());  // пустой путь именует scope, а не сигнал
    EXPECT_FALSE(top->find("cpu").valid());

    EXPECT_EQ(top->findScope("cpu").fullPath(), "top.cpu");
    EXPECT_EQ(top->findScope("").id(), top->id());  // пустой путь — сам scope
    EXPECT_FALSE(top->findScope("nope").valid());
}
