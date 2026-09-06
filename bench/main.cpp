// Замеры ленивого пути wasafe. Прогоны секундного масштаба на синтетическом
// дампе: разбор источника, повторное открытие, обходы курсором, точечные
// запросы, стоимость контрольных сумм. Все случаи детерминированы
// (фиксированное зерно генератора).
//
//   wasafe-bench                 все случаи, таблица для чтения
//   wasafe-bench --csv           то же машиночитаемо
//   wasafe-bench --only=cursor   подмножество по подстроке имени
//   wasafe-bench --scale=0.25    короче прогон (масштабируется длительность)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include "core/crc32.hpp"
#include "io/store_layout.hpp"
#include "report.hpp"
#include "wasafe/io/store.hpp"
#include "wasafe/model/database.hpp"
#include "wasafe/storage/lazy_storage.hpp"
#include "waveform.hpp"

namespace {

using namespace WaSafe;

struct Options {
    double scale = 1.0;
    bool csv = false;
    std::string only;
};

[[nodiscard]] bool wanted(const Options& o, std::string_view name) {
    return o.only.empty() || name.contains(o.only);
}

[[nodiscard]] Bench::Spec specOf(const Options& o) {
    Bench::Spec s;
    s.endTime = static_cast<TimeStamp>(static_cast<double>(s.endTime) * o.scale);
    return s;
}

/// Временный файл, удаляемый в деструкторе.
struct TempStore {
    std::filesystem::path path;

    explicit TempStore(std::string_view name) : path{std::filesystem::temp_directory_path() / name} { drop(); }
    TempStore(const TempStore&) = delete;
    TempStore(TempStore&&) = delete;
    ~TempStore() { drop(); }

    void drop() const {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempStore& operator=(const TempStore&) = delete;
    TempStore& operator=(TempStore&&) = delete;
};

/// Разобрать синтетический дамп в store и вернуть статистику.
Bench::Stats writeStore(const std::filesystem::path& path, const Bench::Spec& spec, std::size_t blockChanges) {
    auto sink = makeIndexingBuilder(path, {.blockChanges = blockChanges});
    const Bench::Stats stats = Bench::generate(*sink, spec);
    sink->finish();
    std::ignore = sink->takeDatabase();
    return stats;
}

/// Листовые узлы дизайна и потоки под ними — вход для всех случаев чтения.
struct Leaves {
    std::vector<NodeId> nodes;
    std::vector<SignalId> streams;
};

[[nodiscard]] Leaves leavesOf(const Database& db) {
    Leaves out;
    out.nodes = db.leafNodes(db.root().id());
    out.streams.reserve(out.nodes.size());
    for (const NodeId n : out.nodes) {
        if (const auto id = db.signalHandle(n).streamId())
            out.streams.push_back(*id);
    }
    return out;
}

/// Смешивание в контрольную сумму. Простое сложение времён тут не годится:
/// метки лежат на регулярной сетке, суммы выходят кратными и вырождаются в ноль.
[[nodiscard]] std::uint64_t mix(std::uint64_t acc, std::uint64_t v) noexcept {
    return acc ^ (v + 0x9e37'79b9'7f4a'7c15ull + (acc << 6u) + (acc >> 2u));
}

/// Детерминированный дребезг для случайных запросов.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_{seed | 1u} {}

    std::uint64_t next() noexcept {
        state_ ^= state_ << 13u;
        state_ ^= state_ >> 7u;
        state_ ^= state_ << 17u;
        return state_;
    }

private:
    std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Разбор источника и открытие: цифры для разговора о замене формата (§7.10)
// ---------------------------------------------------------------------------
void benchIngest(Bench::Report& report, const Options& opts, const std::filesystem::path& corpus) {
    const Bench::Spec spec = specOf(opts);

    if (wanted(opts, "ingest_memory")) {
        auto sink = makeMemoryBuilder();
        const Bench::Timer timer;
        const Bench::Stats stats = Bench::generate(*sink, spec);
        sink->finish();
        const Database db = sink->takeDatabase();
        const double ms = timer.ms();

        report.add("ingest_memory")
                .num("changes", static_cast<double>(stats.changes), 0)
                .num("ms", ms)
                .num("M_chg/s", static_cast<double>(stats.changes) / ms / 1000.0, 2);
    }

    if (wanted(opts, "ingest_store")) {
        const Bench::Timer timer;
        const Bench::Stats stats = writeStore(corpus, spec, 4096);
        const double ms = timer.ms();
        const auto bytes = static_cast<double>(std::filesystem::file_size(corpus));

        report.add("ingest_store")
                .num("changes", static_cast<double>(stats.changes), 0)
                .num("ms", ms)
                .num("M_chg/s", static_cast<double>(stats.changes) / ms / 1000.0, 2)
                .num("MiB", bytes / 1048576.0, 1)
                .num("B/chg", bytes / static_cast<double>(stats.changes), 2);
    }

    if (wanted(opts, "open_store")) {
        const Bench::Timer timer;
        const Database db = openStore(corpus);
        const double ms = timer.ms();
        report.add("open_store").num("ms", ms, 2).num("signals", static_cast<double>(db.hierarchy().signalCount()), 0);
    }
}

// ---------------------------------------------------------------------------
// Курсоры: нужна ли специализация мультикурсора в LazyStorage (§7.3)
// ---------------------------------------------------------------------------
void benchCursors(Bench::Report& report, const Options& opts, const std::filesystem::path& corpus) {
    if (!wanted(opts, "cursor"))
        return;

    const Database db = openStore(corpus);
    const Leaves leaves = leavesOf(db);
    const TimeRange range = db.timeRange();

    for (const std::size_t n : {std::size_t{16}, std::size_t{64}, std::size_t{256}}) {
        if (n > leaves.nodes.size())
            continue;
        const std::span<const NodeId> nodes{leaves.nodes.data(), n};

        // Пакетный курсор: слияние N подкурсоров по времени. Блоки читаются
        // вперемешку между потоками — именно это и предлагалось оптимизировать.
        std::uint64_t batchSeen = 0;
        double batchMs = 0;
        {
            const Bench::Timer timer;
            auto cur = db.changes(nodes, range);
            while (cur.next())
                batchSeen = mix(batchSeen, static_cast<std::uint64_t>(cur->time));
            batchMs = timer.ms();
        }

        // Последовательно по одному потоку: блоки каждого читаются подряд —
        // это верхняя граница «идеально последовательного» доступа.
        std::uint64_t serialSeen = 0;
        double serialMs = 0;
        {
            const Bench::Timer timer;
            for (const NodeId node : nodes) {
                auto cur = db.changes(node, range);
                while (cur.next())
                    serialSeen = mix(serialSeen, static_cast<std::uint64_t>(cur->time));
            }
            serialMs = timer.ms();
        }

        report.add(std::format("cursor_batch_{}", n))
                .num("ms", batchMs)
                .num("checksum", static_cast<double>(batchSeen % 100000), 0);
        report.add(std::format("cursor_serial_{}", n))
                .num("ms", serialMs)
                .num("checksum", static_cast<double>(serialSeen % 100000), 0)
                .num("vs_batch", serialMs / batchMs, 2);
    }
}

// ---------------------------------------------------------------------------
// Тот же замер на MemoryStorage: блоков там нет вовсе, поэтому разница
// batch/serial здесь — ЧИСТАЯ цена слияния. Вычитая её из ленивого случая,
// видно, сколько добавляет раскладка блоков.
// ---------------------------------------------------------------------------
void benchCursorsInMemory(Bench::Report& report, const Options& opts) {
    if (!wanted(opts, "cursor_mem"))
        return;

    auto sink = makeMemoryBuilder();
    std::ignore = Bench::generate(*sink, specOf(opts));
    sink->finish();
    const Database db = sink->takeDatabase();

    const Leaves leaves = leavesOf(db);
    const TimeRange range = db.timeRange();

    for (const std::size_t n : {std::size_t{16}, std::size_t{64}, std::size_t{256}}) {
        if (n > leaves.nodes.size())
            continue;
        const std::span<const NodeId> nodes{leaves.nodes.data(), n};

        std::uint64_t batchSeen = 0;
        const Bench::Timer batchTimer;
        {
            auto cur = db.changes(nodes, range);
            while (cur.next())
                batchSeen = mix(batchSeen, static_cast<std::uint64_t>(cur->time));
        }
        const double batchMs = batchTimer.ms();

        std::uint64_t serialSeen = 0;
        const Bench::Timer serialTimer;
        for (const NodeId node : nodes) {
            auto cur = db.changes(node, range);
            while (cur.next())
                serialSeen = mix(serialSeen, static_cast<std::uint64_t>(cur->time));
        }
        const double serialMs = serialTimer.ms();

        report.add(std::format("cursor_mem_batch_{}", n))
                .num("ms", batchMs)
                .num("checksum", static_cast<double>(batchSeen % 100000), 0);
        report.add(std::format("cursor_mem_serial_{}", n))
                .num("ms", serialMs)
                .num("checksum", static_cast<double>(serialSeen % 100000), 0)
                .num("vs_batch", serialMs / batchMs, 2);
    }
}

// ---------------------------------------------------------------------------
// prefetch и курсоры: курсор идёт мимо кэша, значит прогрев для него — холостая
// работа. Вопрос §7.3 — насколько дорогая.
// ---------------------------------------------------------------------------
void benchPrefetch(Bench::Report& report, const Options& opts, const std::filesystem::path& corpus) {
    if (!wanted(opts, "prefetch"))
        return;

    Database db = openStore(corpus);
    const Leaves leaves = leavesOf(db);
    const TimeRange range = db.timeRange();
    const std::size_t n = std::min<std::size_t>(64, leaves.nodes.size());
    const std::span<const NodeId> nodes{leaves.nodes.data(), n};
    const std::span<const SignalId> streams{leaves.streams.data(), std::min(n, leaves.streams.size())};

    std::uint64_t seen = 0;
    const Bench::Timer warm;
    db.storage().prefetch(streams, range);
    const double prefetchMs = warm.ms();

    const Bench::Timer sweep;
    auto cur = db.changes(nodes, range);
    while (cur.next())
        seen = mix(seen, static_cast<std::uint64_t>(cur->time));
    const double cursorMs = sweep.ms();

    report.add("prefetch_then_cursor")
            .num("prefetch_ms", prefetchMs)
            .num("cursor_ms", cursorMs)
            .num("cached_MiB", static_cast<double>(db.storage().cachedBytes()) / 1048576.0, 1)
            .num("checksum", static_cast<double>(seen % 100000), 0);
}

// ---------------------------------------------------------------------------
// Точечный доступ и размер кэша: сценарий GUI «ведём курсором по осциллограмме»
// ---------------------------------------------------------------------------
void benchValueAt(Bench::Report& report, const Options& opts, const std::filesystem::path& corpus) {
    if (!wanted(opts, "value_at"))
        return;

    constexpr std::size_t kQueries = 50'000;

    for (const std::size_t cacheMiB : {std::size_t{1}, std::size_t{16}, std::size_t{256}}) {
        const Database db = openStore(corpus, {.cacheBytes = cacheMiB << 20});
        const Leaves leaves = leavesOf(db);
        const TimeRange range = db.timeRange();
        const auto span = static_cast<std::uint64_t>(range.end - range.begin);

        Rng rng{777};
        std::uint64_t seen = 0;
        const Bench::Timer timer;
        for (std::size_t q = 0; q < kQueries; ++q) {
            const NodeId node = leaves.nodes[rng.next() % leaves.nodes.size()];
            const auto t = static_cast<TimeStamp>(range.begin + static_cast<TimeStamp>(rng.next() % span));
            seen = mix(seen, static_cast<std::uint64_t>(db.valueAt(node, t).kind()));
        }
        const double ms = timer.ms();

        report.add(std::format("value_at_random_{}MiB", cacheMiB))
                .num("us/query", ms * 1000.0 / static_cast<double>(kQueries), 2)
                .num("ms", ms)
                .num("cached_MiB", static_cast<double>(db.storage().cachedBytes()) / 1048576.0, 1)
                .num("checksum", static_cast<double>(seen % 100000), 0);
    }

    // Ведение курсора: время растёт монотонно, сигналы фиксированы — попадания
    // в кэш должны быть почти стопроцентными.
    const Database db = openStore(corpus);
    const Leaves leaves = leavesOf(db);
    const TimeRange range = db.timeRange();
    const std::size_t visible = std::min<std::size_t>(64, leaves.nodes.size());
    constexpr std::size_t kSteps = 2000;

    std::uint64_t seen = 0;
    const Bench::Timer timer;
    for (std::size_t step = 0; step < kSteps; ++step) {
        const TimeStamp t =
                range.begin + (range.end - range.begin) * static_cast<TimeStamp>(step) / static_cast<TimeStamp>(kSteps);
        for (std::size_t i = 0; i < visible; ++i)
            seen = mix(seen, static_cast<std::uint64_t>(db.valueAt(leaves.nodes[i], t).kind()));
    }
    const double ms = timer.ms();

    report.add("value_at_sweep")
            .num("us/query", ms * 1000.0 / static_cast<double>(kSteps * visible), 3)
            .num("ms", ms)
            .num("queries", static_cast<double>(kSteps * visible), 0)
            .num("checksum", static_cast<double>(seen % 100000), 0);
}

// ---------------------------------------------------------------------------
// Сетка blockChanges: значение по умолчанию (4096) взято из общих соображений
// ---------------------------------------------------------------------------
void benchBlockSize(Bench::Report& report, const Options& opts) {
    if (!wanted(opts, "block_size"))
        return;

    const Bench::Spec spec = specOf(opts);
    for (const std::size_t blockChanges :
            {std::size_t{256}, std::size_t{1024}, std::size_t{4096}, std::size_t{16384}}) {
        const TempStore tmp{std::format("wasafe_bench_bs{}.wsfstore", blockChanges)};

        const Bench::Timer ingestTimer;
        const Bench::Stats stats = writeStore(tmp.path, spec, blockChanges);
        const double ingestMs = ingestTimer.ms();
        const auto bytes = static_cast<double>(std::filesystem::file_size(tmp.path));

        const Database db = openStore(tmp.path);
        const Leaves leaves = leavesOf(db);
        const TimeRange range = db.timeRange();
        const auto span = static_cast<std::uint64_t>(range.end - range.begin);

        Rng rng{4242};
        std::uint64_t seen = 0;
        constexpr std::size_t kQueries = 20'000;
        const Bench::Timer queryTimer;
        for (std::size_t q = 0; q < kQueries; ++q) {
            const NodeId node = leaves.nodes[rng.next() % leaves.nodes.size()];
            const auto t = static_cast<TimeStamp>(range.begin + static_cast<TimeStamp>(rng.next() % span));
            seen = mix(seen, static_cast<std::uint64_t>(db.valueAt(node, t).kind()));
        }
        const double queryMs = queryTimer.ms();

        report.add(std::format("block_size_{}", blockChanges))
                .num("ingest_ms", ingestMs)
                .num("MiB", bytes / 1048576.0, 1)
                .num("B/chg", bytes / static_cast<double>(stats.changes), 2)
                .num("us/query", queryMs * 1000.0 / static_cast<double>(kQueries), 2)
                .num("checksum", static_cast<double>(seen % 100000), 0);
    }
}

// ---------------------------------------------------------------------------
// Пропускная способность самой контрольной суммы. Меряется отдельно от store,
// потому что это единственная цифра, по которой выбирается способ счёта: цена в
// ingest и в сверке блоков — уже её производные. Размеры взяты вокруг типичного
// блока (4096 изменений — это десятки килобайт).
// ---------------------------------------------------------------------------
void benchCrc32(Bench::Report& report, const Options& opts) {
    if (!wanted(opts, "crc32"))
        return;

    // Содержимое на скорость CRC не влияет, но одинаковые байты соблазняют
    // оптимизатор свернуть вычисление.
    std::vector<std::byte> buf(1u << 20);
    Rng rng{2024};
    for (std::byte& b : buf)
        b = static_cast<std::byte>(rng.next() & 0xFFu);

    const auto budget = static_cast<double>(512u << 20) * opts.scale;  // байт на случай

    for (const std::size_t size : {std::size_t{4096}, std::size_t{65536}, std::size_t{1u << 20}}) {
        const std::span<const std::byte> data{buf.data(), size};
        const auto iters = std::max<std::size_t>(1, static_cast<std::size_t>(budget) / size);

        // Цепочка по crc: результат каждой итерации — начальное значение
        // следующей. Без неё компилятор видит, что вызов чистый и аргумент один
        // и тот же, выносит его из цикла, и замер показывает сотни ТБ/с.
        std::uint32_t crc = Crc32::kInit;
        const Bench::Timer timer;
        for (std::size_t i = 0; i < iters; ++i)
            crc = Crc32::update(crc, data);
        const double ms = timer.ms();

        const double bytes = static_cast<double>(size) * static_cast<double>(iters);
        report.add(std::format("crc32_{}KiB", size / 1024))
                .num("GB/s", bytes / (ms / 1000.0) / 1e9, 2)
                .num("ms", ms)
                .num("checksum", static_cast<double>(crc % 100000), 0);
    }
}

// ---------------------------------------------------------------------------
// Сквозная цена включённой сверки блоков: тот же обход по тем же данным,
// отличается только verifyBlocks. По этой колонке видно, можно ли когда-нибудь
// включить проверку по умолчанию.
// ---------------------------------------------------------------------------
void benchVerifyBlocks(Bench::Report& report, const Options& opts, const std::filesystem::path& corpus) {
    if (!wanted(opts, "verify"))
        return;

    const auto sweep = [&corpus](bool verify, std::uint64_t& seen) {
        const Database db = openStore(corpus, {}, verify);
        const Leaves leaves = leavesOf(db);
        const std::size_t n = std::min<std::size_t>(64, leaves.nodes.size());
        const std::span<const NodeId> nodes{leaves.nodes.data(), n};

        const Bench::Timer timer;
        auto cur = db.changes(nodes, db.timeRange());
        while (cur.next())
            seen = mix(seen, static_cast<std::uint64_t>(cur->time));
        return timer.ms();
    };

    // Холостой проход: иначе первый замер платит за прогрев page cache, и
    // накладные сверки вышли бы заниженными.
    std::uint64_t warmSeen = 0;
    std::ignore = sweep(false, warmSeen);

    std::uint64_t plainSeen = 0;
    const double plainMs = sweep(false, plainSeen);
    std::uint64_t verifySeen = 0;
    const double verifyMs = sweep(true, verifySeen);

    report.add("verify_blocks")
            .num("plain_ms", plainMs)
            .num("verify_ms", verifyMs)
            .num("overhead", verifyMs / plainMs, 2)
            .text("checksum", plainSeen == verifySeen ? "ok" : "РАЗОШЁЛСЯ");
}

// ---------------------------------------------------------------------------
// Потолок метаданных: ленивый режим ограничивает память
// ЗНАЧЕНИЙ (LazyStorageOptions::cacheBytes), а иерархия и геометрия блоков
// грузятся целиком и потолка не имеют. Дизайн с широкой развёрткой массивов —
// тот случай, где это упирается первым: каждый элемент unpacked-массива стоит
// отдельного узла со своим потоком.
//
// Память меряется дважды. meta_MiB/idx_MiB — аналитический счёт библиотеки: он
// разложим по статьям, но видит только capacity контейнеров, то есть НИЖНЯЯ
// граница. rss_MiB — что за это заплатила ОС; разница и есть цена аллокатора.
// ---------------------------------------------------------------------------

/// Размер секции метаданных внутри store. Последние kFooterSize байт — футер
/// (metaOffset, metaSize, crc32, магия), всё явным little-endian, поэтому число
/// собирается сдвигами, а не чтением в тип.
[[nodiscard]] std::uint64_t metaSectionBytes(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in)
        return 0;
    const auto size = static_cast<std::uint64_t>(in.tellg());
    if (size < StoreLayout::kFooterSize)
        return 0;

    std::array<std::uint8_t, StoreLayout::kFooterSize> footer{};
    in.seekg(static_cast<std::streamoff>(size - StoreLayout::kFooterSize));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — istream::read требует char*
    in.read(reinterpret_cast<char*>(footer.data()), static_cast<std::streamsize>(footer.size()));
    if (in.gcount() != static_cast<std::streamsize>(footer.size()))
        return 0;

    std::uint64_t metaSize = 0;
    for (std::size_t i = 8; i-- > 0;)  // второе поле футера, старший байт первым
        metaSize = (metaSize << 8u) | footer[8 + i];
    return metaSize;
}

/// Строки отчёта про память: доли статей считаются от аналитического total.
void reportMemory(Bench::Report::Row row, const Hierarchy::MemoryUse& use, std::size_t indexBytes, std::size_t rssDelta,
        std::uint32_t nodes, std::uint32_t liveStreams) {
    const auto total = static_cast<double>(use.total());
    // B/node — цена одного узла ИЕРАРХИИ; индекс сюда не подмешан, потому что
    // растёт он не с узлами, а с потоками, которые реально переключаются
    // (молчащий поток блоков не заводит). Его цена — отдельной колонкой B/str.
    row.num("nodes", nodes, 0)
            .num("meta_MiB", total / 1048576.0, 1)
            .num("maps_%", 100.0 * static_cast<double>(use.indexes) / total)
            .num("names_%", 100.0 * static_cast<double>(use.names) / total)
            .num("B/node", total / nodes, 0)
            .num("idx_MiB", static_cast<double>(indexBytes) / 1048576.0, 1)
            .num("streams", liveStreams, 0);
    if (indexBytes != 0)
        row.num("B/str", static_cast<double>(indexBytes) / liveStreams, 0);
    if (rssDelta != 0)
        row.num("rss_MiB", static_cast<double>(rssDelta) / 1048576.0, 1);
}

void benchMeta(Bench::Report& report, const Options& opts) {
    if (!wanted(opts, "meta"))
        return;

    Bench::MetaSpec spec;
    // --scale масштабирует длительность дампа (specOf), а здесь дорога сама
    // РАЗВЁРТКА, и число узлов случай масштабирует сам — как это делает crc32.
    // Нижняя граница нужна, чтобы --scale=0.05 в CI оставался осмысленным.
    spec.nodes =
            std::max<std::uint32_t>(4096, static_cast<std::uint32_t>(static_cast<double>(spec.nodes) * opts.scale));

    const TempStore tmp{"wasafe_bench_meta.wsfstore"};

    if (wanted(opts, "meta_build")) {
        Bench::releaseFreedMemory();
        const std::size_t rss0 = Bench::residentBytes();

        auto sink = makeMemoryBuilder();
        const Bench::Timer timer;
        const Bench::Stats stats = Bench::generateMeta(*sink, spec);
        sink->finish();
        const Database db = sink->takeDatabase();
        const double ms = timer.ms();

        const std::size_t rss1 = Bench::residentBytes();
        reportMemory(report.add("meta_build").num("ms", ms), db.hierarchy().memoryUse(), db.storage().metadataBytes(),
                rss1 > rss0 ? rss1 - rss0 : 0, spec.nodes, stats.streams);
    }

    if (wanted(opts, "meta_open")) {
        Bench::Stats stats;
        {
            auto sink = makeIndexingBuilder(tmp.path);
            stats = Bench::generateMeta(*sink, spec);
            sink->finish();
            std::ignore = sink->takeDatabase();
        }

        // Иначе дельта второго случая подряд бессмысленна: аллокатор держит
        // освобождённое у себя, и RSS уже не опускается.
        Bench::releaseFreedMemory();
        const std::size_t rss0 = Bench::residentBytes();

        const Bench::Timer timer;
        const Database db = openStore(tmp.path);
        const double ms = timer.ms();

        const std::size_t rss1 = Bench::residentBytes();
        const auto sect = static_cast<double>(metaSectionBytes(tmp.path));
        reportMemory(report.add("meta_open").num("ms", ms).num("sect_MiB", sect / 1048576.0, 1),
                db.hierarchy().memoryUse(), db.storage().metadataBytes(), rss1 > rss0 ? rss1 - rss0 : 0, spec.nodes,
                stats.streams);
    }
}

/// Тело замеров. Исключения ловит main: наружу из него они выходить не должны,
/// а завершение через std::terminate прячет причину.
int run(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--csv") {
            opts.csv = true;
        }
        else if (arg.starts_with("--scale=")) {
            opts.scale = std::stod(std::string{arg.substr(8)});
        }
        else if (arg.starts_with("--only=")) {
            opts.only = std::string{arg.substr(7)};
        }
        else {
            std::print(stderr, "usage: wasafe-bench [--csv] [--scale=X] [--only=substr]\n");
            return 2;
        }
    }

    const Bench::Spec spec = specOf(opts);
    if (!opts.csv) {
        std::print("дамп: {} потоков, {} тиков, ~{} изменений\n\n", spec.streams,
                static_cast<std::int64_t>(spec.endTime), Bench::estimateChanges(spec));
    }

    Bench::Report report;
    const TempStore corpus{"wasafe_bench_corpus.wsfstore"};

    // Корпус нужен случаям чтения, а ingest_store пишет его сам. Если не
    // запрошено ни того, ни другого (скажем, --only=crc32), не пишем вовсе:
    // это секунды на пустом месте.
    const bool corpusNeeded =
            wanted(opts, "cursor") || wanted(opts, "prefetch") || wanted(opts, "value_at") || wanted(opts, "verify");
    if (corpusNeeded && !wanted(opts, "ingest_store"))
        std::ignore = writeStore(corpus.path, spec, 4096);

    benchIngest(report, opts, corpus.path);
    benchCursors(report, opts, corpus.path);
    benchCursorsInMemory(report, opts);
    benchPrefetch(report, opts, corpus.path);
    benchValueAt(report, opts, corpus.path);
    benchBlockSize(report, opts);
    benchCrc32(report, opts);
    benchVerifyBlocks(report, opts, corpus.path);
    benchMeta(report, opts);

    report.print(opts.csv);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Обработчики намеренно на fputs, а не на std::print: форматирование само
    // может бросить, и тогда исключение ушло бы уже из обработчика.
    try {
        return run(argc, argv);
    }
    catch (const std::exception& e) {
        std::fputs("ошибка: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 1;
    }
    catch (...) {
        std::fputs("ошибка: неизвестное исключение\n", stderr);
        return 1;
    }
}
