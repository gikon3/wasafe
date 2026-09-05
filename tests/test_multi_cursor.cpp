#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wasafe/io/builder.hpp"
#include "wasafe/model/database.hpp"
#include "wasafe/storage/decoded_block.hpp"
#include "wasafe/storage/lazy_storage.hpp"
#include "wasafe/storage/memory_block_source.hpp"
#include "wasafe/storage/memory_storage.hpp"

using namespace WaSafe;

namespace {

/// Записать logic-вектор заданной ширины из строки символов (MSB слева) и отдать
/// его невладеющий вид.
struct LogicScratch {
    LogicVector vec;
    LogicScratch(std::uint32_t width, std::string_view bits) : vec{width} { vec.assignFromChars(bits); }
    [[nodiscard]] ValueView view() const { return vec; }
};

/// Одно наблюдение курсора: кто изменился, когда и на что.
struct Seen {
    std::uint32_t source;
    TimeStamp time;
    std::string value;

    friend bool operator==(const Seen&, const Seen&) = default;
};

std::string logicStr(ValueView v) {
    return v.kind() == ValueKind::LOGIC ? v.logic().toString() : std::string{"<none>"};
}

/// Вычитать курсор до конца. Значение читается ДО следующего next(), то есть
/// ровно в пределах гарантии Cursor о времени жизни ValueView.
std::vector<Seen> drain(Cursor& cur) {
    std::vector<Seen> out;
    while (cur.next())
        out.push_back(Seen{cur.current().source, cur.current().time, logicStr(cur.current().value)});
    return out;
}

std::vector<Seen> drain(ValueCursor& cur) {
    std::vector<Seen> out;
    while (cur.next())
        out.push_back(Seen{cur->source, cur->time, logicStr(cur->value)});
    return out;
}

/// MemoryStorage с тремя 4-битными потоками:
///   s0: 0->0001, 30->0011
///   s1: 10->0010, 30->0100
///   s2: 20->1000
MemoryStorage makeThreeStreams() {
    MemoryStorage st;
    const SignalId s0 = st.createStream(ValueKind::LOGIC, 4);
    const SignalId s1 = st.createStream(ValueKind::LOGIC, 4);
    const SignalId s2 = st.createStream(ValueKind::LOGIC, 4);

    const LogicScratch a{4, "0001"};
    const LogicScratch b{4, "0010"};
    const LogicScratch c{4, "0011"};
    const LogicScratch d{4, "0100"};
    const LogicScratch e{4, "1000"};

    st.append(s0, 0, a.view());
    st.append(s1, 10, b.view());
    st.append(s2, 20, e.view());
    st.append(s0, 30, c.view());
    st.append(s1, 30, d.view());
    st.finalize();
    return st;
}

/// Собрать декодированный блок 4-значной логики из пар (время, "биты").
DecodedBlock makeLogicBlock(const std::vector<std::pair<TimeStamp, std::string>>& changes, std::uint32_t width) {
    DecodedBlock blk{ValueKind::LOGIC, width};
    for (const auto& [t, s] : changes) {
        LogicVector v(width);
        v.assignFromChars(s);
        blk.append(t, ValueView{v});
    }
    return blk;
}

void putBlock(MemoryBlockSource& src, SignalIndex& idx, SignalId id, std::uint64_t offset,
        const std::vector<std::pair<TimeStamp, std::string>>& changes, TimeRange span) {
    const auto raw = encodeBlock(makeLogicBlock(changes, 4));
    src.put(offset, raw);
    idx.addBlock(id,
            BlockRef{.time = span,
                    .offset = offset,
                    .storedSize = static_cast<std::uint32_t>(raw.size()),
                    .rawSize = static_cast<std::uint32_t>(raw.size())});
}

}  // namespace

// Пакетный курсор storage сливает потоки по времени и помечает источник индексом
// в запрошенном span.
TEST(MultiCursor, MemoryStorageMergesByTime) {
    const MemoryStorage st = makeThreeStreams();

    const std::vector<SignalId> ids{SignalId{0}, SignalId{1}, SignalId{2}};
    auto cur = st.openCursor(ids, {0, 100});

    EXPECT_EQ(drain(*cur),
            (std::vector<Seen>{
                    {0, 0, "0001"},
                    {1, 10, "0010"},
                    {2, 20, "1000"},
                    {0, 30, "0011"},
                    {1, 30, "0100"},
            }));
}

// Порядок в span — это и порядок разрешения совпадающих времён.
TEST(MultiCursor, TieBreakFollowsRequestOrder) {
    const MemoryStorage st = makeThreeStreams();

    // s0 и s1 меняются оба в момент 30. Прямой порядок: сначала s0.
    const std::vector<SignalId> straight{SignalId{0}, SignalId{1}};
    auto direct = st.openCursor(straight, {30, 40});
    EXPECT_EQ(drain(*direct), (std::vector<Seen>{{0, 30, "0011"}, {1, 30, "0100"}}));

    // Обратный порядок span меняет и порядок выдачи, и нумерацию источников.
    const std::vector<SignalId> reversed{SignalId{1}, SignalId{0}};
    auto swapped = st.openCursor(reversed, {30, 40});
    EXPECT_EQ(drain(*swapped), (std::vector<Seen>{{0, 30, "0100"}, {1, 30, "0011"}}));
}

// Вырожденные входы: пустой span, пустой диапазон, неизвестный поток в середине.
TEST(MultiCursor, DegenerateInputs) {
    const MemoryStorage st = makeThreeStreams();

    auto none = st.openCursor(std::span<const SignalId>{}, {0, 100});
    EXPECT_FALSE(none->next());

    const std::vector<SignalId> ids{SignalId{0}, SignalId{1}};
    auto emptyRange = st.openCursor(ids, {50, 50});
    EXPECT_FALSE(emptyRange->next());

    // Неизвестный поток не срывает слияние: он просто не даёт изменений, а
    // нумерация остальных сохраняет свои позиции в span.
    const std::vector<SignalId> withGap{SignalId{0}, SignalId{42}, SignalId{2}};
    auto gapped = st.openCursor(withGap, {0, 100});
    EXPECT_EQ(drain(*gapped), (std::vector<Seen>{{0, 0, "0001"}, {2, 20, "1000"}, {0, 30, "0011"}}));
}

// Тот же контракт на ленивом storage, со слиянием через границу блока.
// Заодно проверяет, что using-объявление не потеряло пакетную перегрузку.
TEST(MultiCursor, LazyStorageAcrossBlockBoundary) {
    const SignalId s0{0};
    const SignalId s1{1};

    auto src = std::make_unique<MemoryBlockSource>();
    SignalIndex idx;
    idx.setTimeScale({.exponent = -12, .scale = 1});
    idx.setTimeRange({0, 50});
    // У s0 два блока, у s1 один — слияние обязано пройти границу блока s0.
    putBlock(*src, idx, s0, 0, {{0, "0001"}, {20, "0010"}}, {0, 30});
    putBlock(*src, idx, s0, 1000, {{30, "0100"}}, {30, 50});
    putBlock(*src, idx, s1, 2000, {{10, "1000"}, {40, "1111"}}, {10, 50});

    const LazyStorage st{std::move(idx), std::move(src)};

    const std::vector<SignalId> ids{s0, s1};
    auto cur = st.openCursor(ids, {0, 50});

    EXPECT_EQ(drain(*cur),
            (std::vector<Seen>{
                    {0, 0, "0001"},
                    {1, 10, "1000"},
                    {0, 20, "0010"},
                    {0, 30, "0100"},
                    {1, 40, "1111"},
            }));
}

// Database::changes(span) поверх смеси: простой лист, packed-член (SliceCursor)
// и unpacked-композит (внутреннее слияние). source — индекс узла в span, а не
// индекс листа внутри композита.
TEST(MultiCursor, DatabaseMixedNodes) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId clk = b->declareVar("clk", makeScalar());
    // struct packed { logic[7:0] addr; logic valid; } — один поток на структуру.
    Type const reqT = makeStruct(
            {
                    StructMember{"addr", makeVector(7, 0), /*bit_offset*/ 1},
                    StructMember{"valid", makeScalar(), /*bit_offset*/ 0},
            },
            /*packed=*/true);
    const SignalId req = b->declareVar("req", reqT);
    // logic[3:0] mem[0:1] — у каждого элемента свой поток (id 2 и 3).
    const SignalId mem = b->declareVar("mem", makeArray(makeVector(3, 0), 0, 1));
    ASSERT_FALSE(mem.valid());
    b->endScope();
    b->headerDone();

    const LogicScratch lo{1, "0"};
    const LogicScratch hi{1, "1"};
    const LogicScratch r0{9, "101001011"};  // addr=10100101, valid=1
    const LogicScratch r1{9, "101001010"};  // addr тот же, valid=0
    const LogicScratch m0{4, "0001"};
    const LogicScratch m1{4, "0010"};

    const SignalId e0{2};
    const SignalId e1{3};

    b->setTime(0);
    b->valueChange(clk, lo.view());
    b->valueChange(req, r0.view());
    b->setTime(10);
    b->valueChange(e0, m0.view());
    b->setTime(20);
    b->valueChange(req, r1.view());  // меняется только valid, addr — нет
    b->setTime(30);
    b->valueChange(clk, hi.view());
    b->valueChange(e1, m1.view());
    b->finish();

    auto db = b->takeDatabase();

    const auto clkSig = db.find("top.clk");
    const auto addrSig = db.find("top.req.addr");
    const auto memSig = db.find("top.mem");
    ASSERT_TRUE(clkSig);
    ASSERT_TRUE(addrSig);
    ASSERT_TRUE(memSig);

    const std::vector<NodeId> nodes{clkSig->node(), addrSig->node(), memSig->node()};
    auto cur = db.changes(nodes, {0, 100});

    // addr виден один раз (в 20 менялся сосед по вектору, а не он сам), оба
    // элемента mem приходят под общим индексом 2 — своего узла в span.
    EXPECT_EQ(drain(cur),
            (std::vector<Seen>{
                    {0, 0, "0"},
                    {1, 0, "10100101"},
                    {2, 10, "0001"},
                    {0, 30, "1"},
                    {2, 30, "0010"},
            }));
}

// Одиночный композит в span нумеруется как ОДИН источник, а не по листьям —
// в отличие от changes(NodeId), где source это индекс листа в leafNodes().
TEST(MultiCursor, CompositeNumbersDifferBetweenOverloads) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId mem = b->declareVar("mem", makeArray(makeVector(3, 0), 0, 1));
    ASSERT_FALSE(mem.valid());
    b->endScope();
    b->headerDone();

    const LogicScratch m0{4, "0001"};
    const LogicScratch m1{4, "0010"};
    b->setTime(0);
    b->valueChange(SignalId{0}, m0.view());
    b->setTime(10);
    b->valueChange(SignalId{1}, m1.view());
    b->finish();

    auto db = b->takeDatabase();
    const auto memSig = db.find("top.mem");
    ASSERT_TRUE(memSig);

    // Одноузловой курсор: source — индекс листа в leafNodes(mem).
    auto perLeaf = db.changes(memSig->node(), {0, 100});
    EXPECT_EQ(drain(perLeaf), (std::vector<Seen>{{0, 0, "0001"}, {1, 10, "0010"}}));

    // Span-версия: source — индекс узла в span, композит целиком под номером 0.
    const std::vector<NodeId> nodes{memSig->node()};
    auto perNode = db.changes(nodes, {0, 100});
    EXPECT_EQ(drain(perNode), (std::vector<Seen>{{0, 0, "0001"}, {0, 10, "0010"}}));
}

// Сценарий экспортёра: leafNodes(root) + один курсор дают каждое изменение
// дизайна ровно один раз, в неубывающем порядке времени.
TEST(MultiCursor, ExportWholeDesign) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId clk = b->declareVar("clk", makeScalar());
    b->beginScope("cpu", ScopeKind::MODULE);
    const SignalId pc = b->declareVar("pc", makeVector(3, 0));
    b->endScope();
    b->endScope();
    b->headerDone();

    const LogicScratch lo{1, "0"};
    const LogicScratch hi{1, "1"};
    const LogicScratch p0{4, "0000"};
    const LogicScratch p1{4, "0001"};

    b->setTime(0);
    b->valueChange(clk, lo.view());
    b->valueChange(pc, p0.view());
    b->setTime(10);
    b->valueChange(clk, hi.view());
    b->setTime(20);
    b->valueChange(pc, p1.view());
    b->finish();

    auto db = b->takeDatabase();

    // Обход вложенных scope: сигналы scope идут раньше его подскоупов.
    const std::vector<NodeId> leaves = db.leafNodes(db.root().id());
    ASSERT_EQ(leaves.size(), 2u);
    EXPECT_EQ(db.signalHandle(leaves[0]).fullPath(), "top.clk");
    EXPECT_EQ(db.signalHandle(leaves[1]).fullPath(), "top.cpu.pc");

    auto cur = db.changes(leaves, db.timeRange());
    const std::vector<Seen> seen = drain(cur);

    EXPECT_EQ(seen,
            (std::vector<Seen>{
                    {0, 0, "0"},
                    {1, 0, "0000"},
                    {0, 10, "1"},
                    {1, 20, "0001"},
            }));

    // Каждый source — валидный индекс в запрошенном списке.
    for (const Seen& s : seen)
        EXPECT_LT(s.source, leaves.size());
}
