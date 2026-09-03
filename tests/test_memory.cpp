#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "wasafe/core/exception.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/storage/database.hpp"

using namespace WaSafe;

namespace {

// Удобный помощник: записать logic-вектор заданной ширины из строки символов
// (MSB слева) и отдать его невладеющий вид в value_change.
struct LogicScratch {
    LogicVector vec;
    explicit LogicScratch(std::uint32_t width, std::string_view bits) : vec{width} { vec.assignFromChars(bits); }
    [[nodiscard]] ValueView view() const { return vec; }
};

}  // namespace

// in-memory: скалярный поток value_at / курсор / фронты
TEST(Memory, ScalarValueAtCursorEdges) {
    auto b = makeMemoryBuilder();
    b->setTimeScale({.exponent = static_cast<int>(TimeUnit::NS), .scale = 1});
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId clkId = b->declareVar("clk", makeScalar());
    b->endScope();
    b->headerDone();

    // clk: 0@0, 1@10, 0@20, 1@30
    const LogicScratch lo{1, "0"};
    const LogicScratch hi{1, "1"};
    b->setTime(0);
    b->valueChange(clkId, lo.view());
    b->setTime(10);
    b->valueChange(clkId, hi.view());
    b->setTime(20);
    b->valueChange(clkId, lo.view());
    b->setTime(30);
    b->valueChange(clkId, hi.view());
    b->finish();

    auto db = b->takeDatabase();

    EXPECT_EQ(db.timeRange(), (TimeRange{0, 31}));

    const auto clk = db.find("top.clk");
    ASSERT_TRUE(clk);

    // value_at: до первого изменения — пусто; затем «последнее <= t».
    EXPECT_FALSE(clk->valueAt(-1).valid());
    EXPECT_EQ(clk->valueAt(0).asLogic().toString(), "0");
    EXPECT_EQ(clk->valueAt(5).asLogic().toString(), "0");
    EXPECT_EQ(clk->valueAt(10).asLogic().toString(), "1");
    EXPECT_EQ(clk->valueAt(25).asLogic().toString(), "0");
    EXPECT_EQ(clk->valueAt(99).asLogic().toString(), "1");

    // Курсор отдаёт только реальные изменения внутри [begin, end).
    std::vector<TimeStamp> seen;
    auto cur = clk->changes({5, 30});  // 10 и 20 (30 исключён)
    while (cur.next())
        seen.push_back(cur->time);
    EXPECT_EQ(seen, (std::vector<TimeStamp>{10, 20}));

    // Навигация по фронтам.
    EXPECT_EQ(clk->nextChange(0), 10);
    EXPECT_EQ(clk->nextChange(25), 30);
    EXPECT_EQ(clk->nextChange(30), kNoTime);
    EXPECT_EQ(clk->prevChange(30), 20);
    EXPECT_EQ(clk->prevChange(0), kNoTime);
}

// in-memory: real и string потоки
TEST(Memory, RealAndStringStreams) {
    using namespace std::string_view_literals;

    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId r = b->declareVar("temp", makeReal());
    const SignalId s = b->declareVar("label", makeString());
    b->endScope();
    b->headerDone();

    b->setTime(0);
    b->valueChange(r, 1.5);
    b->valueChange(s, "idle"sv);
    b->setTime(10);
    b->valueChange(r, -2.25);
    b->valueChange(s, "run"sv);
    b->finish();

    auto db = b->takeDatabase();

    EXPECT_DOUBLE_EQ(db.find("top.temp")->valueAt(0).asReal(), 1.5);
    EXPECT_DOUBLE_EQ(db.find("top.temp")->valueAt(10).asReal(), -2.25);
    EXPECT_EQ(db.find("top.label")->valueAt(5).asString(), "idle");
    EXPECT_EQ(db.find("top.label")->valueAt(10).asString(), "run");
}

// in-memory: packed-структура — агрегат и битовые срезы
TEST(Memory, PackedStructAggregateAndSlices) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    // struct packed { logic[7:0] addr; logic valid; } — valid в бите 0, addr в [8:1].
    Type const reqT = makeStruct(
            {
                    StructMember{"addr", makeVector(7, 0), /*bit_offset*/ 1},
                    StructMember{"valid", makeScalar(), /*bit_offset*/ 0},
            },
            /*packed=*/true);
    const SignalId req = b->declareVar("req", reqT);
    ASSERT_TRUE(req.valid());  // packed-композит хранится одним потоком
    b->endScope();
    b->headerDone();

    // Полный 9-битный вектор: addr=0xA5 (10100101), valid=1 => "101001011".
    const LogicScratch v{9, "101001011"};
    b->setTime(0);
    b->valueChange(req, v.view());
    b->finish();

    auto db = b->takeDatabase();

    const auto sig = db.find("top.req");
    ASSERT_TRUE(sig);
    EXPECT_TRUE(sig->isComposite());
    EXPECT_EQ(sig->childCount(), 2u);

    // Битовые срезы извлекаются из общего потока по абсолютным смещениям.
    EXPECT_EQ(db.find("top.req.addr")->valueAt(0).asLogic().toString(), "10100101");
    EXPECT_EQ(db.find("top.req.valid")->valueAt(0).asLogic().toString(), "1");

    // value_at композита — агрегат из членов в порядке объявления.
    const Value agg = sig->valueAt(0);
    ASSERT_TRUE(agg.isAggregate());
    ASSERT_EQ(agg.asAggregate().size(), 2u);
    EXPECT_EQ(agg.asAggregate()[0].asLogic().toString(), "10100101");  // addr
    EXPECT_EQ(agg.asAggregate()[1].asLogic().toString(), "1");         // valid
}

// in-memory: у packed-члена changes()/фронты отдают ТОЛЬКО его собственные
// изменения — моменты, когда менялись соседние биты общего вектора, не в счёт.
TEST(Memory, PackedMemberChangesAndEdges) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    Type const reqT = makeStruct(
            {
                    StructMember{"addr", makeVector(7, 0), /*bit_offset*/ 1},
                    StructMember{"valid", makeScalar(), /*bit_offset*/ 0},
            },
            /*packed=*/true);
    const SignalId req = b->declareVar("req", reqT);
    b->endScope();
    b->headerDone();

    // Поток req меняется в 0/10/20, но addr — только в 0 и 20, а valid — в 0 и 10.
    const LogicScratch v0{9, "101001011"};  // addr=10100101, valid=1
    const LogicScratch v1{9, "101001010"};  // addr тот же,   valid=0
    const LogicScratch v2{9, "000000010"};  // addr=00000001, valid тот же
    b->setTime(0);
    b->valueChange(req, v0.view());
    b->setTime(10);
    b->valueChange(req, v1.view());
    b->setTime(20);
    b->valueChange(req, v2.view());
    b->finish();

    auto db = b->takeDatabase();

    // Собрать (время, значение) из курсора члена.
    const auto collect = [](ValueCursor cur) {
        std::vector<std::pair<TimeStamp, std::string>> out;
        while (cur.next())
            out.emplace_back(cur->time, cur->value.logic().toString());
        return out;
    };

    const auto addr = db.find("top.req.addr");
    const auto valid = db.find("top.req.valid");
    ASSERT_TRUE(addr);
    ASSERT_TRUE(valid);

    using Rec = std::pair<TimeStamp, std::string>;
    EXPECT_EQ(collect(addr->changes({0, 100})), (std::vector<Rec>{{0, "10100101"}, {20, "00000001"}}));
    EXPECT_EQ(collect(valid->changes({0, 100})), (std::vector<Rec>{{0, "1"}, {10, "0"}}));

    // Значения курсора совпадают с точечным чтением.
    EXPECT_EQ(addr->valueAt(20).asLogic().toString(), "00000001");
    EXPECT_EQ(valid->valueAt(10).asLogic().toString(), "0");

    // Значение переносится в окно: изменение потока в t=10 не является
    // изменением addr, даже если предыдущего состояния внутри окна нет.
    EXPECT_EQ(collect(addr->changes({5, 100})), (std::vector<Rec>{{20, "00000001"}}));
    EXPECT_EQ(collect(valid->changes({5, 100})), (std::vector<Rec>{{10, "0"}}));

    // Навигация по фронтам согласована со списком моментов каждого члена.
    EXPECT_EQ(addr->nextChange(-1), 0);
    EXPECT_EQ(addr->nextChange(0), 20);
    EXPECT_EQ(addr->nextChange(20), kNoTime);
    EXPECT_EQ(addr->prevChange(100), 20);
    EXPECT_EQ(addr->prevChange(20), 0);
    EXPECT_EQ(addr->prevChange(0), kNoTime);

    EXPECT_EQ(valid->nextChange(-1), 0);
    EXPECT_EQ(valid->nextChange(0), 10);
    EXPECT_EQ(valid->nextChange(10), kNoTime);
    EXPECT_EQ(valid->prevChange(100), 10);
    EXPECT_EQ(valid->prevChange(10), 0);
    EXPECT_EQ(valid->prevChange(0), kNoTime);

    // Композит целиком по-прежнему отдаёт все моменты своего потока.
    std::vector<TimeStamp> whole;
    auto cur = db.find("top.req")->changes({0, 100});
    while (cur.next())
        whole.push_back(cur->time);
    EXPECT_EQ(whole, (std::vector<TimeStamp>{0, 10, 20}));
    EXPECT_EQ(db.find("top.req")->nextChange(0), 10);
    EXPECT_EQ(db.find("top.req")->prevChange(100), 20);
}

// То же для элемента packed-массива: ord 0 — это [0], и он ложится в СТАРШИЕ
// биты (offset = (count-1-ord)*ew), поэтому фронты младшей половины его не
// касаются.
TEST(Memory, PackedArrayElementChangesAndEdges) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    Type const arrT = makeArray(makeVector(3, 0), 0, 1, /*packed=*/true);
    const SignalId arr = b->declareVar("arr", arrT);
    ASSERT_TRUE(arr.valid());
    b->endScope();
    b->headerDone();

    // [0] = биты [7:4], [1] = биты [3:0]. Меняем только младшую половину.
    const LogicScratch v0{8, "10100011"};
    const LogicScratch v1{8, "10101100"};
    b->setTime(0);
    b->valueChange(arr, v0.view());
    b->setTime(10);
    b->valueChange(arr, v1.view());
    b->finish();

    auto db = b->takeDatabase();

    const auto e0 = db.find("top.arr[0]");
    const auto e1 = db.find("top.arr[1]");
    ASSERT_TRUE(e0);
    ASSERT_TRUE(e1);
    EXPECT_EQ(e0->valueAt(10).asLogic().toString(), "1010");
    EXPECT_EQ(e1->valueAt(10).asLogic().toString(), "1100");

    std::vector<TimeStamp> seen0;
    auto c0 = e0->changes({0, 100});
    while (c0.next())
        seen0.push_back(c0->time);
    EXPECT_EQ(seen0, (std::vector<TimeStamp>{0}));  // [0] изменился только в 0

    std::vector<TimeStamp> seen1;
    auto c1 = e1->changes({0, 100});
    while (c1.next())
        seen1.push_back(c1->time);
    EXPECT_EQ(seen1, (std::vector<TimeStamp>{0, 10}));

    EXPECT_EQ(e0->nextChange(0), kNoTime);
    EXPECT_EQ(e1->nextChange(0), 10);
    EXPECT_EQ(e0->prevChange(100), 0);
    EXPECT_EQ(e1->prevChange(100), 10);
}

// in-memory: unpacked-массив — свои потоки и merge-курсор
TEST(Memory, UnpackedArrayOwnStreamsMergeCursor) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    // logic[3:0] mem[0:1] — распакованный массив: у каждого элемента свой поток.
    Type const memT = makeArray(makeVector(3, 0), 0, 1);
    const SignalId mem = b->declareVar("mem", memT);
    EXPECT_FALSE(mem.valid());  // у unpacked-композита нет единого потока
    b->endScope();
    b->headerDone();

    // Порядок разворачивания определён и остаётся гарантией совместимости:
    // mem[0] -> id 0, mem[1] -> id 1. Выводить id счётом выделений больше не
    // обязательно — есть явная адресация через параметр leaves (тесты ниже).
    const SignalId e0{0};
    const SignalId e1{1};
    const LogicScratch m0a{4, "0001"};
    const LogicScratch m0b{4, "0011"};
    const LogicScratch m1a{4, "0010"};
    const LogicScratch m1b{4, "0100"};
    b->setTime(0);
    b->valueChange(e0, m0a.view());
    b->valueChange(e1, m1a.view());
    b->setTime(10);
    b->valueChange(e1, m1b.view());
    b->setTime(20);
    b->valueChange(e0, m0b.view());
    b->finish();

    auto db = b->takeDatabase();

    // Доступ к элементам единым Signal-API, индексация на любом уровне.
    EXPECT_EQ(db.find("top.mem[0]")->valueAt(0).asLogic().toString(), "0001");
    EXPECT_EQ(db.find("top.mem[1]")->valueAt(10).asLogic().toString(), "0100");

    const auto memSig = db.find("top.mem");
    ASSERT_TRUE(memSig);

    // value_at композита — агрегат из элементов.
    const Value agg = memSig->valueAt(10);
    ASSERT_TRUE(agg.isAggregate());
    ASSERT_EQ(agg.asAggregate().size(), 2u);
    EXPECT_EQ(agg.asAggregate()[0].asLogic().toString(), "0001");  // mem[0] @10 (последнее <=10)
    EXPECT_EQ(agg.asAggregate()[1].asLogic().toString(), "0100");  // mem[1] @10

    // changes композита сливает листовые потоки в хронологическом порядке.
    std::vector<TimeStamp> seen;
    auto cur = memSig->changes({0, 40});
    while (cur.next())
        seen.push_back(cur->time);
    EXPECT_EQ(seen, (std::vector<TimeStamp>{0, 0, 10, 20}));

    // next_change/prev_change композита — ближайший фронт среди элементов.
    EXPECT_EQ(memSig->nextChange(0), 10);
    EXPECT_EQ(memSig->prevChange(15), 10);
}

// Явная адресация элементов: невалидные слоты leaves заполняются выделенными
// потоками, и именно в них потом пишутся значения.
TEST(Memory, ExplicitLeavesFillAllocatedStreams) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    Type const memT = makeArray(makeVector(3, 0), 0, 1);
    ASSERT_EQ(expansionStreamCount(memT), 2u);

    std::vector<SignalId> leaves(expansionStreamCount(memT));
    EXPECT_FALSE(b->declareVar("mem", memT, std::nullopt, leaves).valid());
    ASSERT_TRUE(leaves[0].valid());
    ASSERT_TRUE(leaves[1].valid());
    EXPECT_NE(leaves[0], leaves[1]);
    b->endScope();
    b->headerDone();

    const LogicScratch m0{4, "0001"};
    const LogicScratch m1{4, "0010"};
    b->setTime(0);
    b->valueChange(leaves[0], m0.view());
    b->valueChange(leaves[1], m1.view());
    b->finish();

    auto db = b->takeDatabase();
    EXPECT_EQ(db.find("top.mem[0]")->valueAt(0).asLogic().toString(), "0001");
    EXPECT_EQ(db.find("top.mem[1]")->valueAt(0).asLogic().toString(), "0010");
    EXPECT_TRUE(db.find("top.mem")->valueAt(0).isAggregate());
}

// Валидный слот на входе — алиас на уровне элемента: два элемента делят поток.
TEST(Memory, ExplicitLeavesBindExistingStreamAsAlias) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId shared = b->declareVar("plain", makeVector(3, 0));
    ASSERT_TRUE(shared.valid());

    Type const memT = makeArray(makeVector(3, 0), 0, 1);
    std::vector<SignalId> leaves(expansionStreamCount(memT));
    leaves[1] = shared;  // mem[1] — то же самое, что plain
    std::ignore = b->declareVar("mem", memT, std::nullopt, leaves);
    EXPECT_EQ(leaves[1], shared);
    EXPECT_NE(leaves[0], shared);
    b->endScope();
    b->headerDone();

    const LogicScratch v{4, "1011"};
    b->setTime(0);
    b->valueChange(shared, v.view());
    b->finish();

    auto db = b->takeDatabase();
    EXPECT_EQ(db.find("top.plain")->valueAt(0).asLogic().toString(), "1011");
    EXPECT_EQ(db.find("top.mem[1]")->valueAt(0).asLogic().toString(), "1011");
}

// Вложенный массив: потоков столько, сколько листьев, а порядок span —
// глубина-первым по возрастанию порядкового номера.
TEST(Memory, ExplicitLeavesNestedArrayOrder) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    Type const mT = makeArray(makeArray(makeVector(3, 0), 0, 1), 0, 1);
    // elementCount() внешнего измерения — 2, а потоков 4.
    EXPECT_EQ(mT->elementCount(), 2u);
    ASSERT_EQ(expansionStreamCount(mT), 4u);

    std::vector<SignalId> leaves(expansionStreamCount(mT));
    std::ignore = b->declareVar("m", mT, std::nullopt, leaves);
    b->endScope();
    b->headerDone();

    // Порядок: m[0][0], m[0][1], m[1][0], m[1][1].
    const std::array<std::string_view, 4> bits{"0001", "0010", "0100", "1000"};
    const std::array<std::string_view, 4> paths{"top.m[0][0]", "top.m[0][1]", "top.m[1][0]", "top.m[1][1]"};
    std::vector<LogicScratch> vals;
    vals.reserve(bits.size());
    for (const auto bit : bits)
        vals.emplace_back(4, bit);
    b->setTime(0);
    for (std::size_t i = 0; i < leaves.size(); ++i)
        b->valueChange(leaves[i], vals[i].view());
    b->finish();

    auto db = b->takeDatabase();
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto sig = db.find(paths[i]);
        ASSERT_TRUE(sig) << paths[i];
        EXPECT_EQ(sig->valueAt(0).asLogic().toString(), std::string{bits[i]}) << paths[i];
    }
}

// Убывающий диапазон: ordinal 0 — это indexLeft, то есть [3], а не [0].
TEST(Memory, ExplicitLeavesDescendingRangeStartsAtIndexLeft) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    Type const busT = makeArray(makeScalar(), 3, 0);
    std::vector<SignalId> leaves(expansionStreamCount(busT));
    ASSERT_EQ(leaves.size(), 4u);
    std::ignore = b->declareVar("bus", busT, std::nullopt, leaves);
    b->endScope();
    b->headerDone();

    const LogicScratch one{1, "1"};
    const LogicScratch zero{1, "0"};
    b->setTime(0);
    b->valueChange(leaves[0], one.view());  // должен быть bus[3]
    for (std::size_t i = 1; i < leaves.size(); ++i)
        b->valueChange(leaves[i], zero.view());
    b->finish();

    auto db = b->takeDatabase();
    EXPECT_EQ(db.find("top.bus[3]")->valueAt(0).asLogic().toString(), "1");
    EXPECT_EQ(db.find("top.bus[0]")->valueAt(0).asLogic().toString(), "0");
}

// Packed-элемент покрывается ОДНИМ потоком: его члены потоков не получают.
TEST(Memory, ExpansionCountCoversPackedElementWithSingleStream) {
    Type const packedT = makeStruct(
            {
                    StructMember{"addr", makeVector(7, 0), /*bit_offset*/ 1},
                    StructMember{"valid", makeScalar(), /*bit_offset*/ 0},
            },
            /*packed=*/true);
    EXPECT_TRUE(singleStreamRepresentable(packedT));
    EXPECT_EQ(expansionStreamCount(packedT), 0u);  // сам представим одним потоком

    Type const arrT = makeArray(packedT, 0, 3);
    EXPECT_EQ(expansionStreamCount(arrT), 4u);  // по потоку на элемент, а не на член

    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    std::vector<SignalId> leaves(expansionStreamCount(arrT));
    std::ignore = b->declareVar("arr", arrT, std::nullopt, leaves);
    b->endScope();
    b->headerDone();

    const LogicScratch v{9, "101001011"};
    b->setTime(0);
    b->valueChange(leaves[2], v.view());
    b->finish();

    auto db = b->takeDatabase();
    EXPECT_EQ(db.find("top.arr[2].addr")->valueAt(0).asLogic().toString(), "10100101");
    EXPECT_EQ(db.find("top.arr[2].valid")->valueAt(0).asLogic().toString(), "1");
}

// Неверный размер span бросает ДО единого изменения состояния: builder остаётся
// пригодным, и следующий корректный вызов работает.
TEST(Memory, ExplicitLeavesWrongSizeThrowsWithoutSideEffects) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    Type const memT = makeArray(makeVector(3, 0), 0, 1);

    std::vector<SignalId> tooShort(1);
    EXPECT_THROW(std::ignore = b->declareVar("mem", memT, std::nullopt, tooShort), Exception);
    std::vector<SignalId> tooLong(3);
    EXPECT_THROW(std::ignore = b->declareVar("mem", memT, std::nullopt, tooLong), Exception);

    // Тип, представимый одним потоком, непустой span не принимает: его поток
    // задаётся параметром alias.
    std::vector<SignalId> one(1);
    EXPECT_THROW(std::ignore = b->declareVar("scalar", makeScalar(), std::nullopt, one), Exception);

    std::vector<SignalId> leaves(expansionStreamCount(memT));
    std::ignore = b->declareVar("mem", memT, std::nullopt, leaves);
    b->endScope();
    b->headerDone();
    b->finish();

    auto db = b->takeDatabase();
    // Ни один неудавшийся вызов не оставил узла в иерархии.
    EXPECT_TRUE(db.find("top.mem[1]"));
    EXPECT_FALSE(db.find("top.scalar"));
    EXPECT_EQ(db.root().scope("top").signalCount(), 1u);
}

// Packed-поддерево span НЕ расходует: даже если внутри packed-композита окажется
// unpacked-узел (в SV так нельзя, но модель типов это допускает), потоки его
// листьев выделяет ядро, а leaves покрывает ровно элементы верхнего уровня.
TEST(Memory, ExplicitLeavesAreNotConsumedInsidePackedSubtree) {
    Type const inner = makeArray(makeScalar(), 0, 1);  // unpacked внутри packed
    Type const packedT = makeStruct({StructMember{"a", inner, /*bit_offset*/ 0}}, /*packed=*/true);
    Type const outerT = makeArray(packedT, 0, 1);
    ASSERT_EQ(expansionStreamCount(outerT), 2u);  // по потоку на packed-элемент

    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    std::vector<SignalId> leaves(expansionStreamCount(outerT));
    ASSERT_NO_THROW(std::ignore = b->declareVar("s", outerT, std::nullopt, leaves));
    ASSERT_TRUE(leaves[0].valid());
    ASSERT_TRUE(leaves[1].valid());
    EXPECT_NE(leaves[0], leaves[1]);
    b->endScope();
    b->headerDone();
    b->finish();

    auto db = b->takeDatabase();
    // Элементы верхнего уровня — ровно потоки из span, в объявленном порядке.
    EXPECT_EQ(db.find("top.s[0]")->streamId(), std::optional{leaves[0]});
    EXPECT_EQ(db.find("top.s[1]")->streamId(), std::optional{leaves[1]});
    // А узлы внутри packed-элемента получили потоки от ядра, не из span.
    const auto nested = db.find("top.s[0].a[0]");
    ASSERT_TRUE(nested);
    EXPECT_TRUE(nested->hasOwnStream());
    EXPECT_NE(nested->streamId(), std::optional{leaves[0]});
    EXPECT_NE(nested->streamId(), std::optional{leaves[1]});
}

// То же, что LazyBackend.NarrowValueFillsTailWithX, но для MemoryStorage и через
// публичный путь: ровно так внешний парсер и роняет процесс, если отдаст в
// valueChange значение уже объявленного потока.
TEST(Memory, NarrowValueFillsTailWithX) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId bus = b->declareVar("bus", makeVector(7, 0));
    b->endScope();
    b->headerDone();

    b->setTime(0);
    b->valueChange(bus, LogicScratch{8, "00001111"}.view());
    b->setTime(10);
    b->valueChange(bus, LogicScratch{4, "1010"}.view());
    b->finish();
    const Database db = b->takeDatabase();

    const auto sig = db.find("top.bus");
    ASSERT_TRUE(sig);
    EXPECT_EQ(sig->valueAt(0).asLogic().toString(), "00001111");
    // Хвост — «биты не записаны», то есть X: та же семантика, что у
    // LogicVectorView::operator[] за пределами ширины.
    EXPECT_EQ(sig->valueAt(10).asLogic().toString(), "xxxx1010");
}

// Поток шире одного слова: append копирует бит-планы словами, а срез члена,
// пересекающего границу 64 бит, склеивает соседние слова сдвигом. Значения
// содержат все четыре состояния — b-план участвует наравне с a-планом.
TEST(Memory, WideStreamAndSliceAcrossWordBoundary) {
    constexpr std::uint32_t kWidth = 100;

    // Детерминированный узор из 0/1/x/z, старший разряд слева.
    std::string bits;
    for (std::uint32_t i = 0; i < kWidth; ++i)
        bits.push_back("01xz"[(i * 3u + i / 7u) % 4u]);

    // Подстрока члена [offset, offset+width) в записи «MSB слева».
    const auto member = [&bits](std::uint32_t offset, std::uint32_t width) {
        return bits.substr(bits.size() - offset - width, width);
    };

    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    Type const wideT = makeStruct(
            {
                    StructMember{"hi", makeVector(15, 0), /*bit_offset*/ 84},   // 84..99
                    StructMember{"mid", makeVector(23, 0), /*bit_offset*/ 60},  // 60..83, через границу слова
                    StructMember{"lo", makeVector(59, 0), /*bit_offset*/ 0},    // 0..59
            },
            /*packed=*/true);
    const SignalId w = b->declareVar("w", wideT);
    b->endScope();
    b->headerDone();

    // Второе значение отличается ровно одним битом внутри mid: остальные члены
    // изменением считаться не должны.
    std::string other = bits;
    other[other.size() - 1 - 70] = other[other.size() - 1 - 70] == '1' ? '0' : '1';

    b->setTime(0);
    b->valueChange(w, LogicScratch{kWidth, bits}.view());
    b->setTime(10);
    b->valueChange(w, LogicScratch{kWidth, other}.view());
    b->finish();
    auto db = b->takeDatabase();

    // Весь поток вернулся разряд в разряд: члены объявлены от старших битов к
    // младшим и покрывают ширину целиком, поэтому агрегат склеивается в исходную
    // строку.
    const auto whole = db.find("top.w");
    ASSERT_TRUE(whole);
    const auto joined = [](const Value& v) {
        std::string out;
        for (const Value& part : v.asAggregate())
            out += part.asLogic().toString();
        return out;
    };
    EXPECT_EQ(joined(whole->valueAt(0)), bits);
    EXPECT_EQ(joined(whole->valueAt(10)), other);

    const auto hi = db.find("top.w.hi");
    const auto mid = db.find("top.w.mid");
    const auto lo = db.find("top.w.lo");
    ASSERT_TRUE(hi);
    ASSERT_TRUE(mid);
    ASSERT_TRUE(lo);

    EXPECT_EQ(hi->valueAt(0).asLogic().toString(), member(84, 16));
    EXPECT_EQ(mid->valueAt(0).asLogic().toString(), member(60, 24));
    EXPECT_EQ(lo->valueAt(0).asLogic().toString(), member(0, 60));

    // Изменился только mid — у остальных членов второй записи нет.
    const auto times = [](ValueCursor cur) {
        std::vector<TimeStamp> out;
        while (cur.next())
            out.push_back(cur->time);
        return out;
    };
    EXPECT_EQ(times(mid->changes({0, 100})), (std::vector<TimeStamp>{0, 10}));
    EXPECT_EQ(times(hi->changes({0, 100})), (std::vector<TimeStamp>{0}));
    EXPECT_EQ(times(lo->changes({0, 100})), (std::vector<TimeStamp>{0}));
}

// Пословное копирование не имеет права утащить в поток разряды сверх его
// ширины: чужой вид зануления за своей шириной не обещает, а значение шире
// объявленного потока просто обрезается. Мусор не виден в toString (он и так
// маскирует), поэтому проверяется сравнение векторов — оно смотрит на СЛОВА.
TEST(Memory, AppendMasksBitsAboveStreamWidth) {
    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId bus = b->declareVar("bus", makeVector(7, 0));  // ровно 8 бит
    b->endScope();
    b->headerDone();

    // Вид ширины 8 поверх слова, у которого старшие разряды заняты мусором.
    const std::uint64_t dirty = 0xDEAD'BEEF'CAFE'0000ull | 0b0101'0101ull;
    const LogicVectorView dirtyView{&dirty, nullptr, 8};

    // Значение ШИРЕ потока: лишние разряды отбрасываются, как и раньше.
    const LogicScratch wide{12, "111100110011"};

    b->setTime(0);
    b->valueChange(bus, dirtyView);
    b->setTime(10);
    b->valueChange(bus, wide.view());
    b->finish();
    const Database db = b->takeDatabase();

    const auto sig = db.find("top.bus");
    ASSERT_TRUE(sig);

    const LogicScratch clean{8, "01010101"};
    EXPECT_EQ(sig->valueAt(0).asLogic(), clean.vec);
    EXPECT_EQ(sig->valueAt(0).asLogic().toString(), "01010101");

    const LogicScratch low{8, "00110011"};
    EXPECT_EQ(sig->valueAt(10).asLogic(), low.vec);
    EXPECT_EQ(sig->valueAt(10).asLogic().toString(), "00110011");
}

// Узкое значение в широкий поток: незаписанные разряды слота дозаполняются
// целыми словами, поэтому проверяется случай, где хвост из X перекрывает и
// границу слова, и целое слово целиком.
TEST(Memory, NarrowValueFillsWholeWordsWithX) {
    constexpr std::uint32_t kWidth = 200;
    constexpr std::uint32_t kGiven = 70;

    auto b = makeMemoryBuilder();
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId bus = b->declareVar("bus", makeVector(kWidth - 1, 0));
    b->endScope();
    b->headerDone();

    // Значение задевает только слово 0 и один разряд слова 1: слово 2 и хвост
    // слова 1 обязаны целиком стать X.
    const std::string given(kGiven, '1');
    b->setTime(0);
    b->valueChange(bus, LogicScratch{kGiven, given}.view());
    b->finish();
    const Database db = b->takeDatabase();

    const auto sig = db.find("top.bus");
    ASSERT_TRUE(sig);
    EXPECT_EQ(sig->valueAt(0).asLogic().toString(), std::string(kWidth - kGiven, 'x') + given);
}
