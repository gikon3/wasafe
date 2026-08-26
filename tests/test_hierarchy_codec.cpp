#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/byte_io.hpp"
#include "io/hierarchy_codec.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/storage/database.hpp"
#include "wasafe/types/type.hpp"

using namespace WaSafe;

namespace {

/// Дизайн со всеми видами типов, какие ядро вообще умеет: скаляр, вектор,
/// enum, packed-структура (её члены — проекции одного потока), unpacked-массив
/// packed-структур (у каждого элемента свой поток), union, real, string,
/// алиас и вложенный scope.
Database buildGnarlyDesign() {
    auto sink = makeMemoryBuilder();
    sink->setTimeScale({.exponent = -12, .scale = 1});

    sink->beginScope("top", ScopeKind::MODULE);

    const SignalId clk = sink->declareVar("clk", makeScalar());
    sink->declareVar("clk_mirror", makeScalar(), clk);  // алиас: общий поток
    sink->declareVar("bus", makeVector(7, 0, /*isSigned*/ true));

    // Двухзначные (bit): fourState — единственное, чем они отличаются от logic,
    // и записывается он только для скаляра и вектора.
    sink->declareVar("bit_flag", makeScalar(/*fourState*/ false));
    sink->declareVar("cnt", makeVector(3, 0, /*isSigned*/ false, /*fourState*/ false));

    const Type stateT = std::make_shared<const EnumType>(makeVector(1, 0),
            std::vector<EnumEntry>{{"IDLE", 0}, {"RUN", 1}, {"DONE", 2}});
    sink->declareVar("state", stateT);

    const Type pktT = makeStruct({{"hdr", makeVector(3, 0), 0}, {"flag", makeScalar(), 4}}, /*packed*/ true);
    sink->declareVar("pkt", pktT);

    const Type cellT = makeStruct({{"a", makeVector(7, 0), 0}, {"b", makeScalar(), 8}}, /*packed*/ true);
    const Type memT = makeArray(cellT, 0, 2, /*packed*/ false);
    std::vector<SignalId> leaves(expansionStreamCount(memT));
    sink->declareVar("mem", memT, std::nullopt, leaves);

    const Type unionT = std::make_shared<const StructType>(
            std::vector<StructMember>{{"as_word", makeVector(7, 0), 0}, {"as_pair", cellT, 0}},
            /*packed*/ true, /*isUnion*/ true);
    sink->declareVar("u", unionT);

    sink->declareVar("temp", makeReal());
    sink->declareVar("label", makeString());

    sink->beginScope("sub", ScopeKind::GENERATE_BLOCK);
    sink->declareVar("x", makeVector(15, 0));
    sink->endScope();

    sink->endScope();
    sink->headerDone();
    sink->finish();
    return sink->takeDatabase();
}

/// Рекурсивное сравнение типов. Через toSvString это делать нельзя: он пока
/// заглушка и отдаёт только имя вида (см. TODO в types/type.cpp), поэтому
/// перепутанные границы вектора или смещения членов остались бы незамеченными.
// NOLINTNEXTLINE(misc-no-recursion) — обход графа типов по определению рекурсивен
void expectSameType(const Type& a, const Type& b) {
    ASSERT_EQ(static_cast<bool>(a), static_cast<bool>(b));
    if (!a)
        return;

    ASSERT_EQ(a->kind(), b->kind());
    EXPECT_EQ(a->fourState(), b->fourState());
    EXPECT_EQ(a->bitWidth(), b->bitWidth());
    EXPECT_EQ(a->elementCount(), b->elementCount());

    switch (a->kind()) {
        case TypeKind::VECTOR: {
            const auto* lhs = a->as<VectorType>();
            const auto* rhs = b->as<VectorType>();
            ASSERT_NE(lhs, nullptr);
            ASSERT_NE(rhs, nullptr);
            EXPECT_EQ(lhs->msb(), rhs->msb());
            EXPECT_EQ(lhs->lsb(), rhs->lsb());
            EXPECT_EQ(lhs->isSigned(), rhs->isSigned());
            break;
        }
        case TypeKind::ARRAY: {
            const auto* lhs = a->as<ArrayType>();
            const auto* rhs = b->as<ArrayType>();
            ASSERT_NE(lhs, nullptr);
            ASSERT_NE(rhs, nullptr);
            EXPECT_EQ(lhs->indexLeft(), rhs->indexLeft());
            EXPECT_EQ(lhs->indexRight(), rhs->indexRight());
            EXPECT_EQ(lhs->packed(), rhs->packed());
            expectSameType(lhs->elementType(), rhs->elementType());
            break;
        }
        case TypeKind::STRUCT:
        case TypeKind::UNION: {
            const auto* lhs = a->as<StructType>();
            const auto* rhs = b->as<StructType>();
            ASSERT_NE(lhs, nullptr);
            ASSERT_NE(rhs, nullptr);
            EXPECT_EQ(lhs->packed(), rhs->packed());
            ASSERT_EQ(lhs->members().size(), rhs->members().size());
            for (std::size_t i = 0; i < lhs->members().size(); ++i) {
                EXPECT_EQ(lhs->members()[i].name, rhs->members()[i].name);
                EXPECT_EQ(lhs->members()[i].bitOffset, rhs->members()[i].bitOffset);
                expectSameType(lhs->members()[i].type, rhs->members()[i].type);
            }
            break;
        }
        case TypeKind::ENUM: {
            const auto* lhs = a->as<EnumType>();
            const auto* rhs = b->as<EnumType>();
            ASSERT_NE(lhs, nullptr);
            ASSERT_NE(rhs, nullptr);
            ASSERT_EQ(lhs->entries().size(), rhs->entries().size());
            for (std::size_t i = 0; i < lhs->entries().size(); ++i) {
                EXPECT_EQ(lhs->entries()[i].name, rhs->entries()[i].name);
                EXPECT_EQ(lhs->entries()[i].value, rhs->entries()[i].value);
            }
            expectSameType(lhs->base(), rhs->base());
            break;
        }
        case TypeKind::REAL: {
            const auto* lhs = a->as<RealType>();
            const auto* rhs = b->as<RealType>();
            ASSERT_NE(lhs, nullptr);
            ASSERT_NE(rhs, nullptr);
            EXPECT_EQ(lhs->isShortreal(), rhs->isShortreal());
            break;
        }
        default:
            break;  // SCALAR/STRING исчерпываются общей частью
    }
}

void expectSameHierarchy(const Hierarchy& a, const Hierarchy& b) {
    ASSERT_EQ(a.scopeCount(), b.scopeCount());
    ASSERT_EQ(a.signalCount(), b.signalCount());

    for (std::size_t i = 0; i < a.scopeCount(); ++i) {
        const ScopeId id{static_cast<ScopeId::ValueType>(i)};
        const auto& lhs = a.scopeNode(id);
        const auto& rhs = b.scopeNode(id);
        EXPECT_EQ(lhs.name, rhs.name);
        EXPECT_EQ(lhs.kind, rhs.kind);
        EXPECT_EQ(lhs.parent, rhs.parent);
        EXPECT_EQ(lhs.childScopes, rhs.childScopes);
        EXPECT_EQ(lhs.signals, rhs.signals);
        EXPECT_EQ(a.pathOf(id), b.pathOf(id));
    }

    for (std::size_t i = 0; i < a.signalCount(); ++i) {
        const NodeId id{static_cast<NodeId::ValueType>(i)};
        const auto& lhs = a.signalNode(id);
        const auto& rhs = b.signalNode(id);
        EXPECT_EQ(lhs.name, rhs.name);
        EXPECT_EQ(lhs.scope, rhs.scope);
        EXPECT_EQ(lhs.parent, rhs.parent);
        EXPECT_EQ(lhs.stream, rhs.stream);
        EXPECT_EQ(lhs.children, rhs.children);
        EXPECT_EQ(lhs.projection.has_value(), rhs.projection.has_value());
        if (lhs.projection && rhs.projection) {
            EXPECT_EQ(lhs.projection->offset, rhs.projection->offset);
            EXPECT_EQ(lhs.projection->width, rhs.projection->width);
        }
        expectSameType(lhs.type, rhs.type);
        EXPECT_EQ(a.pathOf(id), b.pathOf(id));

        // Путь должен разрешаться обратно в тот же узел.
        const auto found = b.findSignal(b.pathOf(id));
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, id);
    }
}

Hierarchy roundTrip(const Hierarchy& src) {
    std::vector<std::byte> buf;
    ByteWriter w{buf};
    encodeHierarchy(w, src);
    ByteReader r{buf};
    return decodeHierarchy(r);
}

}  // namespace

// Полный round-trip: дерево, типы, потоки и проекции переживают сериализацию.
TEST(HierarchyCodec, RoundTrip) {
    const Database db = buildGnarlyDesign();
    const Hierarchy restored = roundTrip(db.hierarchy());
    expectSameHierarchy(db.hierarchy(), restored);
}

// Разделение типов не теряется: элементы массива ссылаются на ОДИН дескриптор,
// иначе таблица типов раздувалась бы линейно по числу элементов.
TEST(HierarchyCodec, TypeSharingSurvives) {
    const Database db = buildGnarlyDesign();
    const Hierarchy restored = roundTrip(db.hierarchy());

    const auto mem = restored.findSignal("top.mem");
    ASSERT_TRUE(mem.has_value());
    const auto& memNode = restored.signalNode(*mem);
    ASSERT_EQ(memNode.children.size(), 3u);

    const Type& first = restored.signalNode(memNode.children[0]).type;
    for (const NodeId child : memNode.children)
        EXPECT_EQ(restored.signalNode(child).type.get(), first.get());

    // И у самого массива элементный тип — тот же объект.
    const auto* arr = memNode.type->as<ArrayType>();
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->elementType().get(), first.get());
}

// Union и enum восстанавливаются со своей семантикой, а не как обычная структура.
TEST(HierarchyCodec, UnionAndEnumKeepKind) {
    const Database db = buildGnarlyDesign();
    const Hierarchy restored = roundTrip(db.hierarchy());

    const auto u = restored.findSignal("top.u");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(restored.signalNode(*u).type->kind(), TypeKind::UNION);

    const auto state = restored.findSignal("top.state");
    ASSERT_TRUE(state.has_value());
    const auto* et = restored.signalNode(*state).type->as<EnumType>();
    ASSERT_NE(et, nullptr);
    EXPECT_EQ(et->labelOf(1), "RUN");
    EXPECT_EQ(et->base()->bitWidth(), 2u);
}

// Повреждённый поток даёт исключение, а не мусорную иерархию.
TEST(HierarchyCodec, RejectsCorruptStream) {
    const Database db = buildGnarlyDesign();
    std::vector<std::byte> buf;
    ByteWriter w{buf};
    encodeHierarchy(w, db.hierarchy());

    for (const std::size_t cut : {std::size_t{0}, buf.size() / 3, buf.size() / 2, buf.size() - 1}) {
        std::vector<std::byte> truncated{buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(cut)};
        ByteReader r{truncated};
        EXPECT_THROW((void)decodeHierarchy(r), Exception) << "cut = " << cut;
    }
}
