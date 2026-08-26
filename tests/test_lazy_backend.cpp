#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "wasafe/storage/block_source.hpp"
#include "wasafe/storage/decoded_block.hpp"
#include "wasafe/storage/lazy_storage.hpp"
#include "wasafe/storage/memory_block_source.hpp"

using namespace WaSafe;

namespace {

/// Собрать декодированный блок 4-значной логики из пар (время, "биты").
DecodedBlock makeLogicBlock(std::vector<std::pair<TimeStamp, std::string>> changes, std::uint32_t width) {
    DecodedBlock b{ValueKind::LOGIC, width};
    for (auto& [t, s] : changes) {
        LogicVector v(width);
        v.assignFromChars(s);
        b.append(t, ValueView{v});
    }
    return b;
}

/// Backend поверх двух блоков в памяти:
///   блок0 [0,30):  0->0001, 10->0010, 20->0100
///   блок1 [30,50): 30->1000, 40->1111
LazyStorage makeBackend(SignalId id) {
    const auto b0 = encodeBlock(makeLogicBlock({{0, "0001"}, {10, "0010"}, {20, "0100"}}, 4));
    const auto b1 = encodeBlock(makeLogicBlock({{30, "1000"}, {40, "1111"}}, 4));

    auto src = std::make_unique<MemoryBlockSource>();
    src->put(/*offset*/ 0, b0);
    src->put(/*offset*/ 1000, b1);

    SignalIndex idx;
    idx.setTimeScale({.exponent = -12, .scale = 1});
    idx.setTimeRange({0, 50});
    idx.addBlock(id,
            BlockRef{.time = {0, 30},
                    .offset = 0,
                    .storedSize = static_cast<std::uint32_t>(b0.size()),
                    .rawSize = static_cast<std::uint32_t>(b0.size())});
    idx.addBlock(id,
            BlockRef{.time = {30, 50},
                    .offset = 1000,
                    .storedSize = static_cast<std::uint32_t>(b1.size()),
                    .rawSize = static_cast<std::uint32_t>(b1.size())});

    return LazyStorage{std::move(idx), std::move(src)};
}

std::string logicStr(ValueView v) {
    return v.kind() == ValueKind::LOGIC ? v.logic().toString() : std::string{"<none>"};
}

}  // namespace

// LazyStorage::value_at — точечный поиск с переносом между блоками
TEST(LazyBackend, ValueAtAcrossBlocks) {
    const SignalId s{0};
    auto be = makeBackend(s);

    EXPECT_EQ(be.valueAt(s, -1).kind(), ValueKind::NONE);  // до начала данных
    EXPECT_EQ(logicStr(be.valueAt(s, 0)), "0001");
    EXPECT_EQ(logicStr(be.valueAt(s, 5)), "0001");  // держится между изменениями
    EXPECT_EQ(logicStr(be.valueAt(s, 15)), "0010");
    EXPECT_EQ(logicStr(be.valueAt(s, 25)), "0100");
    EXPECT_EQ(logicStr(be.valueAt(s, 30)), "1000");  // граница блока
    EXPECT_EQ(logicStr(be.valueAt(s, 45)), "1111");
    EXPECT_EQ(logicStr(be.valueAt(s, 999)), "1111");  // после конца
}

// LazyStorage::open_cursor — ленивый обход диапазона через границу блока
TEST(LazyBackend, CursorAcrossBlockBoundary) {
    const SignalId s{0};
    auto be = makeBackend(s);

    auto cur = be.openCursor(s, {5, 35});
    std::vector<std::pair<TimeStamp, std::string>> got;
    while (cur->next())
        got.emplace_back(cur->current().time, logicStr(cur->current().value));

    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], (std::pair<TimeStamp, std::string>{10, "0010"}));
    EXPECT_EQ(got[1], (std::pair<TimeStamp, std::string>{20, "0100"}));
    EXPECT_EQ(got[2], (std::pair<TimeStamp, std::string>{30, "1000"}));  // уже из второго блока
}

// LazyStorage::open_cursor — пустой диапазон
TEST(LazyBackend, CursorEmptyRange) {
    const SignalId s{0};
    auto be = makeBackend(s);
    auto cur = be.openCursor(s, {100, 200});
    EXPECT_FALSE(cur->next());
}

// LazyStorage::next_change / prev_change
TEST(LazyBackend, NextPrevChange) {
    const SignalId s{0};
    auto be = makeBackend(s);

    EXPECT_EQ(be.nextChange(s, 10), 20);
    EXPECT_EQ(be.nextChange(s, 20), 30);  // переход в следующий блок
    EXPECT_EQ(be.nextChange(s, 40), kNoTime);
    EXPECT_EQ(be.nextChange(s, 45), kNoTime);

    EXPECT_EQ(be.prevChange(s, 25), 20);
    EXPECT_EQ(be.prevChange(s, 30), 20);
    EXPECT_EQ(be.prevChange(s, 15), 10);
    EXPECT_EQ(be.prevChange(s, 35), 30);
    EXPECT_EQ(be.prevChange(s, 0), kNoTime);
}

// LazyStorage — учёт и сброс кэша
TEST(LazyBackend, CacheAccounting) {
    const SignalId s{0};
    auto be = makeBackend(s);

    EXPECT_EQ(be.cachedBytes(), 0u);
    std::ignore = be.valueAt(s, 15);
    EXPECT_GT(be.cachedBytes(), 0u);  // блок загружен в кэш

    be.release({100, 200});  // ни один блок не пересекает — кэш очищен
    EXPECT_EQ(be.cachedBytes(), 0u);
}

// Четырёхзначные значения переживают сериализацию блока
TEST(LazyBackend, FourStateRoundtrip) {
    const auto raw = encodeBlock(makeLogicBlock({{0, "01xz"}, {10, "xxxx"}, {20, "zz01"}}, 4));
    const DecodedBlock got = decodeBlock(raw);

    ASSERT_EQ(got.count(), 3u);
    EXPECT_EQ(logicStr(got.valueAtIndex(0)), "01xz");
    EXPECT_EQ(logicStr(got.valueAtIndex(1)), "xxxx");
    EXPECT_EQ(logicStr(got.valueAtIndex(2)), "zz01");
    EXPECT_EQ(got.times()[0], 0);
    EXPECT_EQ(got.times()[2], 20);
}

// Первое x/z посреди блока не портит уже записанные двухзначные значения:
// bval разворачивается нулями под них.
TEST(LazyBackend, PromotesToFourStateMidBlock) {
    const auto b = makeLogicBlock({{0, "0001"}, {10, "1010"}, {20, "01x1"}, {30, "1111"}}, 4);

    EXPECT_EQ(logicStr(b.valueAtIndex(0)), "0001");
    EXPECT_EQ(logicStr(b.valueAtIndex(1)), "1010");
    EXPECT_EQ(logicStr(b.valueAtIndex(2)), "01x1");
    EXPECT_EQ(logicStr(b.valueAtIndex(3)), "1111");

    // И после кругового рейса через сериализацию тоже.
    const DecodedBlock got = decodeBlock(encodeBlock(b));
    ASSERT_EQ(got.count(), 4u);
    EXPECT_EQ(logicStr(got.valueAtIndex(0)), "0001");
    EXPECT_EQ(logicStr(got.valueAtIndex(2)), "01x1");
    EXPECT_EQ(logicStr(got.valueAtIndex(3)), "1111");
}

// Двухзначный блок не хранит bval-план — он вдвое компактнее четырёхзначного.
TEST(LazyBackend, TwoStateBlockIsSmaller) {
    const auto twoState = makeLogicBlock({{0, "0001"}, {10, "1010"}, {20, "1111"}}, 4);
    const auto fourState = makeLogicBlock({{0, "0001"}, {10, "1010"}, {20, "111x"}}, 4);

    EXPECT_LT(twoState.byteSize(), fourState.byteSize());
    EXPECT_LT(encodeBlock(twoState).size(), encodeBlock(fourState).size());

    // Значения при этом читаются одинаково корректно.
    EXPECT_EQ(logicStr(twoState.valueAtIndex(2)), "1111");
    EXPECT_EQ(logicStr(fourState.valueAtIndex(2)), "111x");
}

// unknown signal id — пустые результаты
TEST(LazyBackend, UnknownSignalId) {
    const SignalId s{0};
    auto be = makeBackend(s);
    const SignalId missing{42};

    EXPECT_EQ(be.valueAt(missing, 10).kind(), ValueKind::NONE);
    EXPECT_EQ(be.nextChange(missing, 0), kNoTime);
    EXPECT_FALSE(be.openCursor(missing, {0, 50})->next());
}
