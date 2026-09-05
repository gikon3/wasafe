#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "wasafe/core/exception.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/io/store.hpp"
#include "wasafe/model/database.hpp"
#include "wasafe/storage/block_source.hpp"
#include "wasafe/storage/decoded_block.hpp"
#include "wasafe/storage/lazy_storage.hpp"

using namespace WaSafe;

namespace {

/// Временный путь, удаляемый в деструкторе.
struct TempPath {
    std::filesystem::path path;

    explicit TempPath(std::string_view name) : path{std::filesystem::temp_directory_path() / name} { cleanup(); }
    TempPath(const TempPath&) = delete;
    TempPath(TempPath&&) = delete;
    ~TempPath() { cleanup(); }

    void cleanup() const {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempPath& operator=(const TempPath&) = delete;
    TempPath& operator=(TempPath&&) = delete;
};

struct LogicScratch {
    LogicVector vec;
    LogicScratch(std::uint32_t width, std::string_view bits) : vec{width} { vec.assignFromChars(bits); }
    [[nodiscard]] ValueView view() const { return vec; }
};

constexpr std::size_t kSignals = 8;
constexpr TimeStamp kChanges = 400;

/// Наполнить приёмник детерминированным дампом: kSignals четырёхбитных потоков,
/// каждый меняется на своей сетке времени. Блоки мелкие, чтобы ленивый путь
/// реально ходил по нескольким блокам на поток.
void generate(Builder& sink) {
    sink.setTimeScale({.exponent = -12, .scale = 1});
    sink.beginScope("top", ScopeKind::MODULE);
    std::vector<SignalId> ids;
    ids.reserve(kSignals);
    for (std::size_t i = 0; i < kSignals; ++i)
        ids.push_back(sink.declareVar("sig" + std::to_string(i), makeVector(3, 0)));
    sink.endScope();
    sink.headerDone();

    for (TimeStamp t = 0; t < kChanges; ++t) {
        sink.setTime(t);
        for (std::size_t i = 0; i < kSignals; ++i) {
            if (t % static_cast<TimeStamp>(i + 1) != 0)
                continue;
            const auto bit = static_cast<unsigned>((t >> i) & 1);
            const LogicScratch v{4, bit != 0 ? "1010" : "0101"};
            sink.valueChange(ids[i], v.view());
        }
    }
    sink.finish();
}

void writeStore(const std::filesystem::path& path) {
    auto sink = makeIndexingBuilder(path, {.blockChanges = 16});
    generate(*sink);
    std::ignore = sink->takeDatabase();
}

Database memoryDatabase() {
    auto sink = makeMemoryBuilder();
    generate(*sink);
    return sink->takeDatabase();
}

/// Перемешивание в контрольную сумму: простое сложение времён вырождается, они
/// лежат на регулярной сетке.
[[nodiscard]] std::uint64_t mix(std::uint64_t acc, std::uint64_t v) noexcept {
    return acc ^ (v + 0x9e37'79b9'7f4a'7c15ULL + (acc << 6U) + (acc >> 2U));
}

/// Полный обход БД: курсоры по всем листьям плюс точечные запросы. Результат
/// обязан не зависеть ни от экземпляра, ни от потока, в котором посчитан.
[[nodiscard]] std::uint64_t sweep(const Database& db) {
    std::uint64_t acc = 0;
    const auto leaves = db.leafNodes(db.root().id());
    for (const NodeId node : leaves) {
        auto cur = db.changes(node, db.timeRange());
        while (cur.next())
            acc = mix(acc, static_cast<std::uint64_t>(cur->time));
        for (TimeStamp t = 0; t < kChanges; t += 37) {
            const Value v = db.valueAt(node, t);
            acc = mix(acc, v.valid() ? static_cast<std::uint64_t>(v.asLogic().toString().size()) : 0);
        }
    }
    return acc;
}

/// Сколько изменений отдаёт полный обход. Нужен как страховка: совпадение
/// контрольных сумм что-то значит только если обход реально читал данные.
[[nodiscard]] std::size_t countChanges(const Database& db) {
    std::size_t n = 0;
    for (const NodeId node : db.leafNodes(db.root().id())) {
        auto cur = db.changes(node, db.timeRange());
        while (cur.next())
            ++n;
    }
    return n;
}

[[nodiscard]] std::vector<SignalId> streamsOf(const Database& db) {
    std::vector<SignalId> out;
    for (const NodeId node : db.leafNodes(db.root().id())) {
        if (const auto id = db.signalHandle(node).streamId())
            out.push_back(*id);
    }
    return out;
}

/// Источник в духе чужого формата, НЕ умеющий дублироваться: duplicate() у него
/// остаётся дефолтным (nullptr), поэтому БД поверх него не размножается.
class UnduplicableSource final : public BlockSource {
public:
    [[nodiscard]] DecodedBlock decode(SignalId /*id*/, const BlockRef& /*ref*/) const override {
        return DecodedBlock{ValueKind::LOGIC, 4};
    }
};

}  // namespace

// Дубликат in-memory БД — ОТДЕЛЬНЫЙ storage поверх ОБЩИХ данных.
TEST(Duplicate, InMemorySharesDataNotObject) {
    const Database db = memoryDatabase();
    const Database copy = db.duplicate();

    ASSERT_GT(countChanges(db), 500U) << "иначе совпадение сумм ничего не доказывает";
    EXPECT_NE(&db.storage(), &copy.storage()) << "storage не должен разделяться между экземплярами";
    EXPECT_EQ(sweep(db), sweep(copy));
}

// Данные переживают исходную БД: дубликат держит их через shared_ptr.
TEST(Duplicate, OutlivesOriginal) {
    const std::uint64_t expected = [] {
        const Database db = memoryDatabase();
        return sweep(db);
    }();

    const Database copy = [] {
        const Database db = memoryDatabase();
        return db.duplicate();  // оригинал умирает здесь
    }();

    EXPECT_EQ(sweep(copy), expected);
}

// Перемещённая БД сообщает об обращении исключением, а не разыменовывает нуль:
// иерархия теперь за shared_ptr, и после перемещения он пуст.
TEST(Duplicate, MovedFromThrowsInsteadOfCrashing) {
    Database db = memoryDatabase();
    const Database moved = std::move(db);

    // Обращение к перемещённому объекту здесь НАМЕРЕННОЕ — оно и есть предмет проверки.
    // NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_THROW(std::ignore = db.hierarchy(), Exception);
    EXPECT_THROW(std::ignore = db.root(), Exception);
    // NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    EXPECT_NO_THROW(std::ignore = moved.root());
}

// У ленивого дубликата СВОЙ LRU: прогрев одного не виден другому.
TEST(Duplicate, LazyHasOwnCache) {
    const TempPath store{"wasafe_dup_cache.wsfstore"};
    writeStore(store.path);

    // Не const: prefetch — подсказка, мутирующая кэш экземпляра.
    Database db = openStore(store.path);
    const Database copy = db.duplicate();

    const auto streams = streamsOf(db);
    ASSERT_FALSE(streams.empty());
    db.storage().prefetch(std::span<const SignalId>{streams}, db.timeRange());

    EXPECT_GT(db.storage().cachedBytes(), 0U);
    EXPECT_EQ(copy.storage().cachedBytes(), 0U) << "кэш дубликата обязан остаться холодным";
    EXPECT_EQ(sweep(db), sweep(copy));
}

// Несколько потоков, у каждого свой дубликат над одним файлом.
TEST(Duplicate, ConcurrentReadsOverStore) {
    const TempPath store{"wasafe_dup_threads.wsfstore"};
    writeStore(store.path);

    const Database db = openStore(store.path);
    const std::uint64_t expected = sweep(db);

    constexpr std::size_t kThreads = 4;
    std::vector<Database> views;
    views.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i)
        views.push_back(db.duplicate());

    std::vector<std::uint64_t> got(kThreads, 0);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i)
        workers.emplace_back([&views, &got, i] { got[i] = sweep(views[i]); });
    for (auto& w : workers)
        w.join();

    for (std::size_t i = 0; i < kThreads; ++i)
        EXPECT_EQ(got[i], expected) << "поток " << i;
}

// То же без файловой системы: in-memory БД тоже размножается на потоки.
TEST(Duplicate, ConcurrentReadsInMemory) {
    const Database db = memoryDatabase();
    const std::uint64_t expected = sweep(db);

    constexpr std::size_t kThreads = 4;
    std::vector<Database> views;
    views.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i)
        views.push_back(db.duplicate());

    std::vector<std::uint64_t> got(kThreads, 0);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i)
        workers.emplace_back([&views, &got, i] { got[i] = sweep(views[i]); });
    for (auto& w : workers)
        w.join();

    for (std::size_t i = 0; i < kThreads; ++i)
        EXPECT_EQ(got[i], expected) << "поток " << i;
}

// Источник, не реализовавший duplicate(), делает БД неразмножаемой — и об этом
// сообщается исключением, а не тихо отдаётся сломанный экземпляр.
TEST(Duplicate, ThrowsWhenSourceCannotDuplicate) {
    const Database header = [] {
        auto sink = makeMemoryBuilder();
        generate(*sink);
        return sink->takeDatabase();
    }();

    SignalIndex idx;
    idx.setTimeScale({.exponent = -12, .scale = 1});
    idx.setTimeRange({0, kChanges});
    const Database db{header.hierarchy(),
            std::make_unique<LazyStorage>(std::move(idx), std::make_unique<UnduplicableSource>())};

    EXPECT_THROW(std::ignore = db.duplicate(), Exception);
}
