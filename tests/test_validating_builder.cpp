#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <vector>

#include "fake_reader.hpp"
#include "wasafe/core/exception.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/io/ingest.hpp"
#include "wasafe/model/database.hpp"
#include "wasafe/types/logic_vector.hpp"

using namespace WaSafe;

namespace {

LogicVector bits(std::uint32_t width, std::string_view chars) {
    LogicVector v{width};
    v.assignFromChars(chars);
    return v;
}

/// Приёмник под проверкой: MemoryBuilder за декоратором. Держит оба, потому что
/// декоратор ссылку на sink не владеет.
struct Guarded {
    std::unique_ptr<Builder> sink = makeMemoryBuilder();
    std::unique_ptr<Builder> guard = makeValidatingBuilder(*sink);

    Builder& operator*() const noexcept { return *guard; }
    Builder* operator->() const noexcept { return guard.get(); }
};

/// Заголовок «top.clk (scalar) + top.data [7:0] + top.temp (real)», доведённый
/// до фазы значений. Возвращает id потоков в том же порядке.
struct Header {
    SignalId clk;
    SignalId data;
    SignalId temp;
};

Header openHeader(Builder& b) {
    b.setTimeScale({.exponent = static_cast<int>(TimeUnit::NS), .scale = 1});
    b.beginScope("top", ScopeKind::MODULE);
    const Header h{
            .clk = b.declareVar("clk", makeScalar()),
            .data = b.declareVar("data", makeVector(7, 0)),
            .temp = b.declareVar("temp", makeReal()),
    };
    b.endScope();
    b.headerDone();
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------
// Корректный сценарий проходит насквозь без изменения поведения.
// ---------------------------------------------------------------------------

TEST(ValidatingBuilder, ValidStreamPassesThrough) {
    Fake::FakeReader plainReader;
    auto plainSink = makeMemoryBuilder();
    const Database plain = ingest(plainReader, *plainSink);

    Fake::FakeReader guardedReader;
    Guarded guarded;
    const Database checked = ingest(guardedReader, *guarded);

    EXPECT_EQ(checked.timeScale(), plain.timeScale());

    // Тот же дизайн и те же значения на тех же метках времени.
    for (const std::string_view path : {"top.clk", "top.data", "top.sub.rst"}) {
        const auto a = plain.find(path);
        const auto b = checked.find(path);
        ASSERT_TRUE(a) << path;
        ASSERT_TRUE(b) << path;
        for (const TimeStamp t : {TimeStamp{0}, TimeStamp{5}, TimeStamp{10}, TimeStamp{20}, TimeStamp{30}})
            EXPECT_EQ(b->valueAt(t).asLogic().toString(), a->valueAt(t).asLogic().toString()) << path << " @" << t;
    }

    const auto temp = checked.find("top.temp");
    ASSERT_TRUE(temp);
    EXPECT_DOUBLE_EQ(temp->valueAt(30).asReal(), 2.5);

    // Алиас (clk_mirror поверх потока clk) декоратор пропускает: поток объявлен.
    const auto mirror = checked.find("top.clk_mirror");
    ASSERT_TRUE(mirror);
    EXPECT_EQ(mirror->valueAt(10).asLogic().toString(), "1");
}

TEST(ValidatingBuilder, EqualTimeStampsAreAllowed) {
    Guarded b;
    const Header h = openHeader(*b);
    b->setTime(10);
    b->valueChange(h.clk, bits(1, "0"));
    // Монотонность — НЕ строгая: несколько изменений в одном моменте штатны.
    EXPECT_NO_THROW(b->setTime(10));
    EXPECT_NO_THROW(b->valueChange(h.data, bits(8, "00001111")));
}

// ---------------------------------------------------------------------------
// Фаза протокола.
// ---------------------------------------------------------------------------

TEST(ValidatingBuilder, RejectsHeaderCallsAfterHeaderDone) {
    Guarded b;
    openHeader(*b);

    EXPECT_THROW((void)b->declareVar("late", makeScalar()), Exception);
    EXPECT_THROW(b->setTimeScale({}), Exception);
    EXPECT_THROW((void)b->beginScope("late", ScopeKind::MODULE), Exception);
    EXPECT_THROW(b->endScope(), Exception);
    EXPECT_THROW(b->headerDone(), Exception);
}

TEST(ValidatingBuilder, RejectsValuePhaseCallsBeforeHeaderDone) {
    Guarded b;
    b->beginScope("top", ScopeKind::MODULE);
    const SignalId clk = b->declareVar("clk", makeScalar());

    EXPECT_THROW(b->setTime(0), Exception);
    EXPECT_THROW(b->valueChange(clk, bits(1, "0")), Exception);
    EXPECT_THROW(b->finish(), Exception);
}

TEST(ValidatingBuilder, RejectsUnbalancedScopes) {
    Guarded b;
    b->beginScope("top", ScopeKind::MODULE);
    b->beginScope("sub", ScopeKind::MODULE);
    b->endScope();
    // «top» остался открытым — иерархия получится не той, что задумывал парсер.
    EXPECT_THROW(b->headerDone(), Exception);
}

TEST(ValidatingBuilder, RejectsEndScopeWithoutBegin) {
    Guarded b;
    b->beginScope("top", ScopeKind::MODULE);
    b->endScope();
    // Сам приёмник лишний endScope молча проглатывает.
    EXPECT_THROW(b->endScope(), Exception);
}

TEST(ValidatingBuilder, RejectsCallsAfterFinish) {
    Guarded b;
    const Header h = openHeader(*b);
    b->setTime(0);
    b->valueChange(h.clk, bits(1, "0"));
    b->finish();

    EXPECT_THROW(b->finish(), Exception);
    EXPECT_THROW(b->setTime(10), Exception);
    EXPECT_THROW(b->valueChange(h.clk, bits(1, "1")), Exception);
}

TEST(ValidatingBuilder, RejectsTakeDatabaseOutOfOrder) {
    Guarded b;
    const Header h = openHeader(*b);
    b->setTime(0);
    b->valueChange(h.clk, bits(1, "0"));

    EXPECT_THROW((void)b->takeDatabase(), Exception);
    b->finish();
    EXPECT_NO_THROW((void)b->takeDatabase());
    EXPECT_THROW((void)b->takeDatabase(), Exception);
}

// ---------------------------------------------------------------------------
// Время.
// ---------------------------------------------------------------------------

TEST(ValidatingBuilder, RejectsTimeGoingBackwards) {
    Guarded b;
    const Header h = openHeader(*b);
    b->setTime(100);
    b->valueChange(h.clk, bits(1, "0"));
    EXPECT_THROW(b->setTime(99), Exception);
}

TEST(ValidatingBuilder, RejectsValueChangeBeforeFirstSetTime) {
    Guarded b;
    const Header h = openHeader(*b);
    // Без setTime у изменения нет метки: приёмник молча запишет его в момент 0.
    EXPECT_THROW(b->valueChange(h.clk, bits(1, "0")), Exception);
}

// ---------------------------------------------------------------------------
// Поток и значение.
// ---------------------------------------------------------------------------

TEST(ValidatingBuilder, RejectsUnknownSignalId) {
    Guarded b;
    openHeader(*b);
    b->setTime(0);

    EXPECT_THROW(b->valueChange(SignalId{}, bits(1, "0")), Exception);      // невалидный
    EXPECT_THROW(b->valueChange(SignalId{9999}, bits(1, "0")), Exception);  // необъявленный
}

TEST(ValidatingBuilder, RejectsKindMismatch) {
    Guarded b;
    const Header h = openHeader(*b);
    b->setTime(0);

    // Без декоратора это std::bad_variant_access из ValueView::logic(),
    // а не WaSafe::Exception, — ровно то, ради чего он и заведён.
    EXPECT_THROW(b->valueChange(h.clk, 1.5), Exception);
    EXPECT_THROW(b->valueChange(h.temp, bits(1, "0")), Exception);
    EXPECT_THROW(b->valueChange(h.data, std::string_view{"nope"}), Exception);
    EXPECT_THROW(b->valueChange(h.clk, ValueView{}), Exception);
}

TEST(ValidatingBuilder, RejectsWidthMismatch) {
    Guarded b;
    const Header h = openHeader(*b);
    b->setTime(0);

    // Без декоратора короткое значение роняет процесс записью за границу
    // (MemoryStorage::Stream::append), а длинное молча обрезается.
    EXPECT_THROW(b->valueChange(h.data, bits(4, "1010")), Exception);
    EXPECT_THROW(b->valueChange(h.data, bits(16, "1010101010101010")), Exception);
    EXPECT_NO_THROW(b->valueChange(h.data, bits(8, "10101010")));
}

// ---------------------------------------------------------------------------
// Привязка потоков: alias и leaves.
// ---------------------------------------------------------------------------

TEST(ValidatingBuilder, RejectsUnknownAlias) {
    Guarded b;
    b->beginScope("top", ScopeKind::MODULE);
    EXPECT_THROW((void)b->declareVar("ghost", makeScalar(), SignalId{9999}), Exception);
    EXPECT_THROW((void)b->declareVar("ghost", makeScalar(), SignalId{}), Exception);

    const SignalId clk = b->declareVar("clk", makeScalar());
    EXPECT_NO_THROW((void)b->declareVar("clk_mirror", makeScalar(), clk));
}

TEST(ValidatingBuilder, RejectsUnknownLeafBinding) {
    Guarded b;
    b->beginScope("top", ScopeKind::MODULE);

    // Unpacked-массив из двух элементов: явная привязка на уровне элемента —
    // тот же алиас, что и alias, поэтому и спрос тот же.
    const Type mem = makeArray(makeVector(7, 0), 0, 1);
    std::vector<SignalId> leaves{SignalId{9999}, SignalId{}};
    EXPECT_THROW((void)b->declareVar("mem", mem, std::nullopt, leaves), Exception);
}

TEST(ValidatingBuilder, TracksStreamsAllocatedByTheCore) {
    Guarded b;
    b->beginScope("top", ScopeKind::MODULE);
    // Пустой span: потоки листьев выделяет ядро, наружу их id не отдаются.
    // Декоратор обязан узнать их всё равно, иначе первая же запись в элемент
    // массива провалится как «поток не объявлен».
    const SignalId owner = b->declareVar("mem", makeArray(makeVector(7, 0), 0, 1));
    EXPECT_FALSE(owner.valid()) << "у unpacked-композита своего потока нет";
    b->endScope();
    b->headerDone();
    b->setTime(0);

    // Id выдаются в порядке разворачивания, начиная с нуля (гарантия
    // Builder::declareVar), поэтому элементы mem — это потоки 0 и 1.
    EXPECT_NO_THROW(b->valueChange(SignalId{0}, bits(8, "00001111")));
    EXPECT_NO_THROW(b->valueChange(SignalId{1}, bits(8, "11110000")));
    EXPECT_THROW(b->valueChange(SignalId{0}, bits(4, "1111")), Exception);
    EXPECT_THROW(b->valueChange(SignalId{2}, bits(8, "00000000")), Exception);
}

TEST(ValidatingBuilder, KnowsPackedCompositeIsOneStream) {
    Guarded b;
    b->beginScope("top", ScopeKind::MODULE);
    // Packed-структура {logic [7:0] hi; logic [7:0] lo;} — ОДИН поток шириной 16,
    // члены живут битовыми срезами внутри него и своих потоков не получают.
    const Type packed = makeStruct(
            {
                    StructMember{.name = "hi", .type = makeVector(7, 0), .bitOffset = 8},
                    StructMember{.name = "lo", .type = makeVector(7, 0), .bitOffset = 0},
            },
            /*packed=*/true);
    const SignalId word = b->declareVar("word", packed);
    ASSERT_TRUE(word.valid());
    b->endScope();
    b->headerDone();
    b->setTime(0);

    EXPECT_NO_THROW(b->valueChange(word, bits(16, "1010101001010101")));
    EXPECT_THROW(b->valueChange(word, bits(8, "10101010")), Exception);
}
