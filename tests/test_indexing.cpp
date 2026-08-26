#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/byte_io.hpp"
#include "wasafe/config.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/io/store.hpp"
#include "wasafe/storage/database.hpp"
#include "wasafe/storage/lazy_storage.hpp"
#include "wasafe/storage/signal_index.hpp"

using namespace WaSafe;

namespace {

/// Временный путь, удаляемый в деструкторе. Спутников у store нет — файл один.
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

/// Индекс переоткрытого store. Проверять записанную геометрию так строже, чем
/// заглядывать в объект в памяти: заодно подтверждается, что файл самодостаточен.
const SignalIndex& indexOfReopened(const Database& db) {
    const auto* lazy = dynamic_cast<const LazyStorage*>(&db.storage());
    EXPECT_NE(lazy, nullptr) << "переоткрытый store обязан быть ленивым";
    return lazy->index();
}

struct LogicScratch {
    LogicVector vec;
    LogicScratch(std::uint32_t width, std::string_view bits) : vec{width} { vec.assignFromChars(bits); }
    [[nodiscard]] ValueView view() const { return vec; }
};

/// Небольшой store с двумя потоками — заготовка для тестов переоткрытия.
void writeSmallStore(const std::filesystem::path& path) {
    auto b = makeIndexingBuilder(path, {.blockChanges = 2});
    b->setTimeScale({.exponent = -12, .scale = 1});
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId d = b->declareVar("data", makeVector(3, 0));
    b->declareVar("temp", makeReal());
    b->endScope();
    b->headerDone();

    const LogicScratch v{4, "0011"};
    b->setTime(0);
    b->valueChange(d, v.view());
    b->setTime(10);
    b->valueChange(d, v.view());
    b->finish();
    std::ignore = b->takeDatabase();
}

/// Прочитать файл целиком.
std::vector<std::byte> readWhole(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    std::vector<std::byte> buf(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — istream::read требует char*
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return buf;
}

void writeWhole(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — ostream::write требует const char*
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

}  // namespace

// SignalIndex: секция пишется и читается без потерь
TEST(Indexing, SectionRoundtrip) {
    SignalIndex idx;
    idx.setTimeScale({.exponent = -9, .scale = 10});
    idx.setTimeRange({0, 1000});
    idx.addBlock(SignalId{0},
            BlockRef{.time = {0, 500}, .offset = 0, .storedSize = 128, .rawSize = 256, .codec = BlockRef::Codec::ZSTD});
    idx.addBlock(SignalId{0},
            BlockRef{.time = {500, 1000}, .offset = 128, .cookie = 42, .storedSize = 64, .rawSize = 200});
    idx.addBlock(SignalId{7}, BlockRef{.time = {0, 1000}, .offset = 192, .storedSize = 300, .rawSize = 300});

    std::vector<std::byte> buf;
    ByteWriter w{buf};
    idx.encode(w);

    ByteReader r{buf};
    const SignalIndex loaded = SignalIndex::decode(r);
    EXPECT_EQ(r.remaining(), 0u) << "секция прочитана не целиком";

    EXPECT_EQ(loaded.timeRange(), (TimeRange{0, 1000}));
    EXPECT_EQ(loaded.timeScale(), (TimeScale{.exponent = -9, .scale = 10}));
    EXPECT_EQ(loaded.streamCount(), 2u);

    const SignalLocator* l0 = loaded.locate(SignalId{0});
    ASSERT_NE(l0, nullptr);
    ASSERT_EQ(l0->blocks.size(), 2u);
    EXPECT_EQ(l0->blocks[0].time, (TimeRange{0, 500}));
    EXPECT_EQ(l0->blocks[0].storedSize, 128u);
    EXPECT_EQ(l0->blocks[0].rawSize, 256u);
    EXPECT_EQ(l0->blocks[0].codec, BlockRef::Codec::ZSTD);
    EXPECT_EQ(l0->blocks[0].cookie, 0u);
    EXPECT_EQ(l0->blocks[1].offset, 128u);
    EXPECT_EQ(l0->blocks[1].cookie, 42u);  // непрозрачное поле переживает запись

    const SignalLocator* l7 = loaded.locate(SignalId{7});
    ASSERT_NE(l7, nullptr);
    EXPECT_EQ(l7->blocks.size(), 1u);
    EXPECT_EQ(l7->blocks[0].time, (TimeRange{0, 1000}));

    EXPECT_EQ(loaded.locate(SignalId{42}), nullptr);
}

// Обрезанная секция не даёт полуразобранного индекса.
TEST(Indexing, SectionRejectsTruncated) {
    SignalIndex idx;
    idx.setTimeRange({0, 100});
    idx.addBlock(SignalId{1}, BlockRef{.time = {0, 100}, .offset = 8, .storedSize = 16, .rawSize = 16});

    std::vector<std::byte> buf;
    ByteWriter w{buf};
    idx.encode(w);

    for (const std::size_t cut : {std::size_t{0}, buf.size() / 2, buf.size() - 1}) {
        std::vector<std::byte> truncated{buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(cut)};
        ByteReader r{truncated};
        EXPECT_THROW((void)SignalIndex::decode(r), Exception) << "cut = " << cut;
    }
}

TEST(Indexing, EndToEndLazyRead) {
    TempPath const tmp{"wasafe_store_e2e.wsfstore"};

    // Мелкие блоки (2 изменения) — заставит создать несколько блоков и
    // проверить обход границ при ленивом чтении.
    auto b = makeIndexingBuilder(tmp.path, {.blockChanges = 2});
    b->setTimeScale({.exponent = static_cast<int>(TimeUnit::PS), .scale = 1});
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId d = b->declareVar("data", makeVector(3, 0));
    const SignalId r = b->declareVar("temp", makeReal());
    b->endScope();
    b->headerDone();

    // data: 5 изменений -> 3 блока (2+2+1). temp: 3 изменения -> 2 блока.
    const LogicScratch v0{4, "0000"};
    const LogicScratch v1{4, "0001"};
    const LogicScratch v2{4, "0010"};
    const LogicScratch v3{4, "0100"};
    const LogicScratch v4{4, "1000"};
    b->setTime(0);
    b->valueChange(d, v0.view());
    b->valueChange(r, 0.5);
    b->setTime(10);
    b->valueChange(d, v1.view());
    b->setTime(20);
    b->valueChange(d, v2.view());
    b->valueChange(r, 1.5);
    b->setTime(30);
    b->valueChange(d, v3.view());
    b->setTime(40);
    b->valueChange(d, v4.view());
    b->valueChange(r, 2.5);
    b->finish();

    auto db = b->takeDatabase();

    EXPECT_EQ(db.timeRange(), (TimeRange{0, 41}));
    EXPECT_EQ(db.timeScale(), (TimeScale{.exponent = static_cast<int>(TimeUnit::PS), .scale = 1}));

    const auto data = db.find("top.data");
    ASSERT_TRUE(data);

    // Точечный доступ через границы блоков (включая «удержание» значения).
    EXPECT_EQ(data->valueAt(0).asLogic().toString(), "0000");
    EXPECT_EQ(data->valueAt(5).asLogic().toString(), "0000");
    EXPECT_EQ(data->valueAt(10).asLogic().toString(), "0001");
    EXPECT_EQ(data->valueAt(25).asLogic().toString(), "0010");  // блок 1, удержание
    EXPECT_EQ(data->valueAt(35).asLogic().toString(), "0100");  // блок 2 (carry между блоками)
    EXPECT_EQ(data->valueAt(40).asLogic().toString(), "1000");
    EXPECT_EQ(data->valueAt(99).asLogic().toString(), "1000");

    // Курсор по диапазону через несколько блоков.
    std::vector<TimeStamp> seen;
    auto cur = data->changes({5, 35});  // 10,20,30
    while (cur.next())
        seen.push_back(cur->time);
    EXPECT_EQ(seen, (std::vector<TimeStamp>{10, 20, 30}));

    // Навигация по фронтам через границы блоков.
    EXPECT_EQ(data->nextChange(0), 10);
    EXPECT_EQ(data->nextChange(15), 20);
    EXPECT_EQ(data->nextChange(40), kNoTime);
    EXPECT_EQ(data->prevChange(35), 30);
    EXPECT_EQ(data->prevChange(5), 0);

    // Второй поток (real) тоже читается лениво.
    EXPECT_DOUBLE_EQ(db.find("top.temp")->valueAt(25).asReal(), 1.5);
    EXPECT_DOUBLE_EQ(db.find("top.temp")->valueAt(40).asReal(), 2.5);

    // Записанная геометрия согласована с построенной в памяти.
    const Database reopened = openStore(tmp.path);
    const SignalIndex& onDisk = indexOfReopened(reopened);
    EXPECT_EQ(onDisk.timeRange(), (TimeRange{0, 41}));
    ASSERT_TRUE(onDisk.locate(d));
    EXPECT_EQ(onDisk.locate(d)->blocks.size(), 3u);  // 5 изменений / 2 на блок
}

// IndexingBuilder: packed-член на ленивом storage — курсор и фронты члена идут
// по блокам потока-предка, включая переход через границу блока.
TEST(Indexing, PackedMemberChangesLazy) {
    TempPath const tmp{"wasafe_store_packed.wsfstore"};

    // По 2 изменения на блок: 3 изменения потока req лягут в два блока.
    auto b = makeIndexingBuilder(tmp.path, {.blockChanges = 2});
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

    const LogicScratch v0{9, "101001011"};  // addr=10100101, valid=1
    const LogicScratch v1{9, "101001010"};  // addr тот же,   valid=0
    const LogicScratch v2{9, "000000010"};  // addr=00000001, valid тот же (граница блока)
    b->setTime(0);
    b->valueChange(req, v0.view());
    b->setTime(10);
    b->valueChange(req, v1.view());
    b->setTime(20);
    b->valueChange(req, v2.view());
    b->finish();

    auto db = b->takeDatabase();

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
    // Перенос значения в окно работает и когда окно начинается внутри блока.
    EXPECT_EQ(collect(addr->changes({5, 100})), (std::vector<Rec>{{20, "00000001"}}));

    EXPECT_EQ(addr->nextChange(0), 20);  // через границу блока
    EXPECT_EQ(addr->nextChange(20), kNoTime);
    EXPECT_EQ(addr->prevChange(100), 20);
    EXPECT_EQ(addr->prevChange(20), 0);
    EXPECT_EQ(valid->nextChange(0), 10);
    EXPECT_EQ(valid->nextChange(10), kNoTime);
    EXPECT_EQ(valid->prevChange(100), 10);
}

// IndexingBuilder: запись идёт ПО ХОДУ разбора, а не в finish()
TEST(Indexing, StreamsToDiskBeforeFinish) {
    TempPath const tmp{"wasafe_store_streaming.wsfstore"};

    auto b = makeIndexingBuilder(tmp.path, {.blockChanges = 8});
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId w = b->declareVar("bus", makeVector(63, 0));
    b->endScope();
    b->headerDone();

    // Объём заведомо больше буфера потока вывода, поэтому байты обязаны дойти
    // до файла ещё до finish(), если запись действительно потоковая.
    constexpr int kN = 20000;
    const LogicScratch a{64, "0000000000000000000000000000000000000000000000000000000000001010"};
    const LogicScratch c{64, "0000000000000000000000000000000000000000000000000000000001011100"};
    for (TimeStamp i = 0; i < kN; ++i) {
        b->setTime(i * 10);
        b->valueChange(w, (i % 2 == 0 ? a : c).view());
    }

    std::error_code ec;
    const auto sizeBeforeFinish = std::filesystem::file_size(tmp.path, ec);
    ASSERT_FALSE(ec);
    EXPECT_GT(sizeBeforeFinish, 0u) << "store пуст до finish() — запись не потоковая";

    b->finish();
    auto db = b->takeDatabase();

    // И результат при этом корректен.
    const auto bus = db.find("top.bus");
    ASSERT_TRUE(bus);
    EXPECT_EQ(bus->valueAt(0).asLogic().toUint64(), 0xAu);
    EXPECT_EQ(bus->valueAt(10).asLogic().toUint64(), 0x5Cu);
    EXPECT_EQ(bus->valueAt(static_cast<TimeStamp>(kN - 1) * 10).asLogic().toUint64(), (kN % 2 == 0 ? 0x5Cu : 0xAu));
    EXPECT_EQ(db.timeRange(), (TimeRange{0, static_cast<TimeStamp>(kN - 1) * 10 + 1}));
}

// IndexingBuilder: потолок буфера дробит блоки, не ломая чтение
TEST(Indexing, BufferBudgetSplitsBlocks) {
    TempPath const tmp{"wasafe_store_budget.wsfstore"};

    // blockChanges заведомо недостижим — блоки могут появиться ТОЛЬКО из-за
    // досрочного сброса по нехватке памяти.
    auto b = makeIndexingBuilder(tmp.path, {.blockChanges = 1'000'000, .bufferBytes = 8192});
    b->beginScope("top", ScopeKind::MODULE);
    std::vector<SignalId> ids;
    ids.reserve(4);
    for (int k = 0; k < 4; ++k)
        ids.push_back(b->declareVar("bus" + std::to_string(k), makeVector(63, 0)));
    b->endScope();
    b->headerDone();

    constexpr int kSteps = 2000;
    const LogicScratch a{64, "0000000000000000000000000000000000000000000000000000000000001010"};
    const LogicScratch c{64, "0000000000000000000000000000000000000000000000000000000001011100"};
    for (TimeStamp i = 0; i < kSteps; ++i) {
        b->setTime(i * 10);
        for (const SignalId id : ids)
            b->valueChange(id, (i % 2 == 0 ? a : c).view());
    }
    b->finish();
    auto db = b->takeDatabase();

    // Досрочные сбросы действительно произошли.
    const Database reopened = openStore(tmp.path);
    const SignalLocator* loc = indexOfReopened(reopened).locate(ids.front());
    ASSERT_NE(loc, nullptr);
    EXPECT_GT(loc->blocks.size(), 1u) << "бюджет не сработал: поток уместился в один блок";

    // Блоки одного потока упорядочены и не пересекаются, между ними — «дыры».
    for (std::size_t i = 1; i < loc->blocks.size(); ++i)
        EXPECT_LE(loc->blocks[i - 1].time.end, loc->blocks[i].time.begin);

    // Чтение через все границы блоков корректно: точечный доступ, удержание
    // значения внутри «дыры» и полный обход курсором.
    const auto bus0 = db.find("top.bus0");
    ASSERT_TRUE(bus0);
    EXPECT_EQ(bus0->valueAt(0).asLogic().toUint64(), 0xAu);
    EXPECT_EQ(bus0->valueAt(5).asLogic().toUint64(), 0xAu);  // удержание между изменениями
    EXPECT_EQ(bus0->valueAt(10).asLogic().toUint64(), 0x5Cu);
    EXPECT_EQ(bus0->valueAt(static_cast<TimeStamp>(kSteps - 1) * 10).asLogic().toUint64(),
            (kSteps % 2 == 0 ? 0x5Cu : 0xAu));

    std::size_t seen = 0;
    auto cur = bus0->changes(kWholeTime);
    while (cur.next())
        ++seen;
    EXPECT_EQ(seen, static_cast<std::size_t>(kSteps));
}

// IndexingBuilder: zstd-сжатие большого блока и распаковка из файла
TEST(Indexing, ZstdCompressLargeBlock) {
    if (!kHasZstd)
        GTEST_SKIP() << "библиотека собрана без zstd (WASAFE_WITH_ZSTD=OFF)";

    TempPath const tmp{"wasafe_store_zstd.wsfstore"};

    // Один крупный блок из хорошо сжимаемых данных: zstd должен реально сжать,
    // и чтение пойдёт через ZSTD_decompress (codec == Zstd в индексе).
    auto b = makeIndexingBuilder(tmp.path, {.blockChanges = 10000});
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId w = b->declareVar("bus", makeVector(63, 0));
    b->endScope();
    b->headerDone();

    constexpr int kN = 2000;
    const LogicScratch a{64, "0000000000000000000000000000000000000000000000000000000000001010"};  // 0xA
    const LogicScratch c{64, "0000000000000000000000000000000000000000000000000000000001011100"};  // 0x5C
    for (TimeStamp i = 0; i < kN; ++i) {
        b->setTime(i * 10);
        b->valueChange(w, (i % 2 == 0 ? a : c).view());
    }
    b->finish();

    auto db = b->takeDatabase();

    const Database reopened = openStore(tmp.path);
    const SignalLocator* loc = indexOfReopened(reopened).locate(w);
    ASSERT_NE(loc, nullptr);
    ASSERT_EQ(loc->blocks.size(), 1u);
    EXPECT_EQ(loc->blocks[0].codec, BlockRef::Codec::ZSTD);        // сжатие сработало
    EXPECT_LT(loc->blocks[0].storedSize, loc->blocks[0].rawSize);  // и реально уменьшило

    // Чтение значений идёт через распаковку zstd из файла.
    const auto bus = db.find("top.bus");
    ASSERT_TRUE(bus);
    EXPECT_EQ(bus->valueAt(0).asLogic().toUint64(), 0xAu);
    EXPECT_EQ(bus->valueAt(10).asLogic().toUint64(), 0x5Cu);
    EXPECT_EQ(bus->valueAt(static_cast<TimeStamp>(kN - 1) * 10).asLogic().toUint64(),
            (kN % 2 == 0 ? 0x5Cu : 0xAu));  // последнее изменение
}

// Файл самодостаточен: Database уничтожена, парсера нет, исходного дампа нет —
// а дизайн и значения читаются заново.
TEST(Indexing, ReopenAfterClose) {
    TempPath const tmp{"wasafe_reopen.wsfstore"};

    {
        auto b = makeIndexingBuilder(tmp.path, {.blockChanges = 2});
        b->setTimeScale({.exponent = -12, .scale = 1});
        b->beginScope("top", ScopeKind::MODULE);
        const SignalId d = b->declareVar("data", makeVector(3, 0));
        const Type pktT = makeStruct({{"hdr", makeVector(3, 0), 0}, {"flag", makeScalar(), 4}}, /*packed*/ true);
        const SignalId pkt = b->declareVar("pkt", pktT);
        b->beginScope("sub", ScopeKind::GENERATE_BLOCK);
        const SignalId x = b->declareVar("x", makeScalar());
        b->endScope();
        b->endScope();
        b->headerDone();

        const LogicScratch d0{4, "0001"};
        const LogicScratch d1{4, "1100"};
        const LogicScratch p0{5, "10110"};
        const LogicScratch x1{1, "1"};

        b->setTime(0);
        b->valueChange(d, d0.view());
        b->valueChange(pkt, p0.view());
        b->valueChange(x, x1.view());
        b->setTime(20);
        b->valueChange(d, d1.view());
        b->finish();
        std::ignore = b->takeDatabase();  // БД тут же уничтожается
    }

    const Database db = openStore(tmp.path);

    EXPECT_EQ(db.timeScale(), (TimeScale{.exponent = -12, .scale = 1}));
    EXPECT_EQ(db.timeRange(), (TimeRange{0, 21}));

    const auto data = db.find("top.data");
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->width(), 4u);
    EXPECT_EQ(data->valueAt(5).asLogic().toString(), "0001");
    EXPECT_EQ(data->valueAt(25).asLogic().toString(), "1100");

    // Packed-член: тип и проекция на поток предка тоже пережили запись.
    const auto flag = db.find("top.pkt.flag");
    ASSERT_TRUE(flag.has_value());
    EXPECT_EQ(flag->width(), 1u);
    EXPECT_EQ(flag->valueAt(5).asLogic().toString(), "1");

    // Вложенный scope на месте.
    const auto x = db.find("top.sub.x");
    ASSERT_TRUE(x.has_value());
    EXPECT_EQ(x->valueAt(5).asLogic().toString(), "1");

    // Курсор по переоткрытому файлу тоже работает.
    auto cur = data->changes({0, 100});
    std::vector<TimeStamp> times;
    while (cur.next())
        times.push_back(cur->time);
    EXPECT_EQ(times, (std::vector<TimeStamp>{0, 20}));
}

// Открытие отвергает всё, что не является дописанным store, — внятной ошибкой.
TEST(Indexing, OpenStoreRejectsBadFiles) {
    EXPECT_THROW((void)openStore("/no/such/wasafe/store.wsfstore"), Exception);

    // Слишком короткий файл.
    TempPath const tiny{"wasafe_tiny.wsfstore"};
    writeWhole(tiny.path, std::vector<std::byte>(4));
    EXPECT_THROW((void)openStore(tiny.path), Exception);

    // Длина подходящая, но это не наш файл: не совпадает магия заголовка.
    TempPath const junk{"wasafe_junk.wsfstore"};
    writeWhole(junk.path, std::vector<std::byte>(128, std::byte{0x5A}));
    EXPECT_THROW((void)openStore(junk.path), Exception);

    TempPath const full{"wasafe_full.wsfstore"};
    writeSmallStore(full.path);
    const auto image = readWhole(full.path);
    ASSERT_GT(image.size(), 32u);
    EXPECT_NO_THROW((void)openStore(full.path));  // эталон открывается

    // Оборванная запись: футера нет.
    TempPath const cut{"wasafe_cut.wsfstore"};
    writeWhole(cut.path, std::span{image}.first(image.size() - 1));
    EXPECT_THROW((void)openStore(cut.path), Exception);

    // Футер на месте, но метаданные обещаны за границей файла.
    TempPath const bent{"wasafe_bent.wsfstore"};
    std::vector<std::byte> bentImage = image;
    for (std::size_t i = 0; i < 8; ++i)
        bentImage[bentImage.size() - 20 + i] = std::byte{0xFF};  // metaOffset = огромный
    writeWhole(bent.path, bentImage);
    EXPECT_THROW((void)openStore(bent.path), Exception);
}
