#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "wasafe/storage/block_source.hpp"
#include "wasafe/storage/decoded_block.hpp"
#include "wasafe/storage/lazy_storage.hpp"

using namespace WaSafe;

namespace {

constexpr std::uint64_t kChunkOffset = 100;  ///< один «чанк» чужого формата на оба потока
constexpr std::uint32_t kWidth = 4;

/// Источник в духе формата со СВОИМ блочным устройством (FST): наш формат блока
/// он не производит вовсе — наследуется прямо от BlockSource, минуя
/// RawBlockSource, и собирает DecodedBlock из собственных структур. Один чанк
/// несёт изменения нескольких потоков, поэтому offset у их блоков общий —
/// различает потоки SignalId, приходящий параметром decode().
class ForeignSource final : public BlockSource {
public:
    using Changes = std::vector<std::pair<TimeStamp, std::string>>;

public:
    void put(SignalId id, std::uint64_t offset, Changes changes) { chunks_[{id, offset}] = std::move(changes); }

    [[nodiscard]] DecodedBlock decode(SignalId id, const BlockRef& ref) const override {
        ++decodeCalls_;
        // Контекст блока источник держит у себя: пара (id, offset) — его ключ.
        // Промах здесь означал бы, что decode получил не тот SignalId.
        const auto it = chunks_.find({id, ref.offset});
        if (it == chunks_.end())
            throw std::logic_error{"ForeignSource: нет данных для (id, offset)"};

        DecodedBlock block{ValueKind::LOGIC, kWidth};
        for (const auto& [time, bits] : it->second) {
            LogicVector v(kWidth);
            v.assignFromChars(bits);
            block.append(time, ValueView{v});
        }
        return block;
    }

    [[nodiscard]] int decodeCalls() const noexcept { return decodeCalls_; }

private:
    mutable int decodeCalls_ = 0;  ///< mutable: decode() — const
    std::map<std::pair<SignalId, std::uint64_t>, Changes> chunks_;
};

/// Ленивое хранилище поверх одного чанка с двумя потоками плюс наблюдатель за
/// источником. LazyStorage неперемещаем — возвращается prvalue (C++17).
struct Backend {
    LazyStorage storage;
    const ForeignSource* source;
};

Backend makeBackend(SignalId a, SignalId b) {
    auto src = std::make_unique<ForeignSource>();
    src->put(a, kChunkOffset, {{0, "0001"}, {10, "0010"}});
    src->put(b, kChunkOffset, {{0, "1111"}, {10, "1100"}});
    const ForeignSource* observer = src.get();

    SignalIndex idx;
    idx.setTimeScale({.exponent = -12, .scale = 1});
    idx.setTimeRange({0, 11});
    // storedSize/rawSize нулевые: источник байты не читает, размеры ему не нужны.
    const BlockRef ref{.time = {0, 11}, .offset = kChunkOffset};
    idx.addBlock(a, ref);
    idx.addBlock(b, ref);  // ТО ЖЕ смещение — блок другого потока в том же чанке

    return Backend{LazyStorage{std::move(idx), std::move(src)}, observer};
}

std::string logicStr(ValueView v) {
    return v.kind() == ValueKind::LOGIC ? v.logic().toString() : std::string{"<none>"};
}

}  // namespace

// Точечный доступ: потоки одного чанка не путаются, хотя offset у них общий.
TEST(ForeignSource, StreamsShareChunkOffset) {
    const SignalId a{0};
    const SignalId b{1};
    auto be = makeBackend(a, b);

    EXPECT_EQ(logicStr(be.storage.valueAt(a, 5)), "0001");
    EXPECT_EQ(logicStr(be.storage.valueAt(b, 5)), "1111");
    EXPECT_EQ(logicStr(be.storage.valueAt(a, 15)), "0010");
    EXPECT_EQ(logicStr(be.storage.valueAt(b, 15)), "1100");
}

// Кэш блоков различает потоки: два блока по общему смещению — две записи,
// повторные обращения к ним источник не беспокоят.
TEST(ForeignSource, CacheKeepsStreamsApart) {
    const SignalId a{0};
    const SignalId b{1};
    auto be = makeBackend(a, b);

    EXPECT_EQ(be.source->decodeCalls(), 0);
    (void)be.storage.valueAt(a, 5);
    (void)be.storage.valueAt(b, 5);
    EXPECT_EQ(be.source->decodeCalls(), 2);

    (void)be.storage.valueAt(a, 15);
    (void)be.storage.valueAt(b, 15);
    EXPECT_EQ(be.source->decodeCalls(), 2);  // оба блока уже в кэше
}

// Курсор одного потока отдаёт только его изменения.
TEST(ForeignSource, CursorPerStream) {
    const SignalId a{0};
    const SignalId b{1};
    auto be = makeBackend(a, b);

    auto collect = [](std::unique_ptr<Cursor> cur) {
        std::vector<std::pair<TimeStamp, std::string>> got;
        while (cur->next())
            got.emplace_back(cur->current().time, logicStr(cur->current().value));
        return got;
    };

    const auto fromA = collect(be.storage.openCursor(a, {0, 100}));
    ASSERT_EQ(fromA.size(), 2u);
    EXPECT_EQ(fromA[0], (std::pair<TimeStamp, std::string>{0, "0001"}));
    EXPECT_EQ(fromA[1], (std::pair<TimeStamp, std::string>{10, "0010"}));

    const auto fromB = collect(be.storage.openCursor(b, {0, 100}));
    ASSERT_EQ(fromB.size(), 2u);
    EXPECT_EQ(fromB[0], (std::pair<TimeStamp, std::string>{0, "1111"}));
    EXPECT_EQ(fromB[1], (std::pair<TimeStamp, std::string>{10, "1100"}));
}

// Пакетный курсор поверх чужого источника: общий хронологический порядок,
// source указывает на поток в переданном span.
TEST(ForeignSource, MergedCursorTagsSource) {
    const SignalId a{0};
    const SignalId b{1};
    auto be = makeBackend(a, b);

    const SignalId ids[] = {a, b};
    auto cur = be.storage.openCursor(ids, {0, 100});

    std::vector<std::tuple<TimeStamp, std::size_t, std::string>> got;
    while (cur->next())
        got.emplace_back(cur->current().time, cur->current().source, logicStr(cur->current().value));

    ASSERT_EQ(got.size(), 4u);
    EXPECT_EQ(got[0], std::make_tuple(TimeStamp{0}, std::size_t{0}, std::string{"0001"}));
    EXPECT_EQ(got[1], std::make_tuple(TimeStamp{0}, std::size_t{1}, std::string{"1111"}));
    EXPECT_EQ(got[2], std::make_tuple(TimeStamp{10}, std::size_t{0}, std::string{"0010"}));
    EXPECT_EQ(got[3], std::make_tuple(TimeStamp{10}, std::size_t{1}, std::string{"1100"}));
}
