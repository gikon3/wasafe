#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>
#include <system_error>

#include "fake_reader.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/io/ingest.hpp"
#include "wasafe/storage/database.hpp"

using namespace WaSafe;

namespace {

/// Временный путь под нормализованный store (удаляется вместе с сайдкаром).
struct TempStore {
    std::filesystem::path path;

    explicit TempStore(std::string_view name) : path{std::filesystem::temp_directory_path() / name} { cleanup(); }
    TempStore(const TempStore&) = delete;
    TempStore(TempStore&&) = delete;
    ~TempStore() { cleanup(); }

    void cleanup() const {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempStore& operator=(const TempStore&) = delete;
    TempStore& operator=(TempStore&&) = delete;
};

/// Содержимое, которое обязан дать FakeReader через любой приёмник.
void checkContents(const Database& db) {
    EXPECT_EQ(db.timeScale(), (TimeScale{.exponent = static_cast<int>(TimeUnit::NS), .scale = 1}));

    const auto clk = db.find("top.clk");
    ASSERT_TRUE(clk);
    EXPECT_EQ(clk->valueAt(0).asLogic().toString(), "0");
    EXPECT_EQ(clk->valueAt(10).asLogic().toString(), "1");
    EXPECT_EQ(clk->valueAt(25).asLogic().toString(), "0");
    EXPECT_EQ(clk->valueAt(35).asLogic().toString(), "1");

    const auto data = db.find("top.data");
    ASSERT_TRUE(data);
    EXPECT_EQ(data->type()->bitWidth(), 8u);
    EXPECT_EQ(data->valueAt(0).asLogic().toString(), "00000000");
    EXPECT_EQ(data->valueAt(25).asLogic().toString(), "10100101");

    const auto temp = db.find("top.temp");
    ASSERT_TRUE(temp);
    EXPECT_EQ(temp->kind(), TypeKind::REAL);
    EXPECT_DOUBLE_EQ(temp->valueAt(0).asReal(), 1.5);
    EXPECT_DOUBLE_EQ(temp->valueAt(35).asReal(), 2.5);

    const auto rst = db.find("top.sub.rst");
    ASSERT_TRUE(rst);
    EXPECT_EQ(rst->valueAt(0).asLogic().toString(), "0");
    EXPECT_EQ(rst->valueAt(25).asLogic().toString(), "1");

    // Алиас: clk_mirror делит поток с clk.
    const auto mirror = db.find("top.clk_mirror");
    ASSERT_TRUE(mirror);
    EXPECT_EQ(mirror->valueAt(10).asLogic().toString(), "1");
    EXPECT_EQ(mirror->valueAt(25).asLogic().toString(), "0");
}

}  // namespace

// ingest: приёмник в ОЗУ
TEST(Ingest, IntoMemoryBuilder) {
    Fake::FakeReader reader;
    auto sink = makeMemoryBuilder();

    checkContents(ingest(reader, *sink));
}

// ingest: приёмник — нормализованный store на диске (ленивое чтение)
TEST(Ingest, IntoIndexingBuilder) {
    TempStore const tmp{"wasafe_ingest.wsfstore"};

    Fake::FakeReader reader;
    auto sink = makeIndexingBuilder(tmp.path, {.blockChanges = 2});

    // Тот же источник через другой приёмник даёт то же содержимое: выбор режима
    // хранения — дело пользователя, а не парсера.
    checkContents(ingest(reader, *sink));
}

// ingestHeader: иерархия без значений
TEST(Ingest, HeaderOnly) {
    Fake::FakeReader reader;
    auto sink = makeMemoryBuilder();

    auto db = ingestHeader(reader, *sink);

    EXPECT_TRUE(db.find("top.clk"));
    EXPECT_TRUE(db.find("top.sub.rst"));
    EXPECT_FALSE(db.find("top.clk")->valueAt(10).valid());
}
