#include "io/hierarchy_codec.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "wasafe/core/exception.hpp"
#include "wasafe/types/type.hpp"

namespace WaSafe {

namespace {

/// Индекс 0 зарезервирован под пустой Type — отдельного флага «типа нет» не нужно.
constexpr std::uint32_t kNoType = 0;

/// Таблица типов: топологический порядок, дети раньше родителей, поэтому при
/// чтении все ссылки уже разрешены.
class TypeTable {
public:
    /// Зарегистрировать тип и его поддерево; возвращает индекс записи.
    // NOLINTNEXTLINE(misc-no-recursion) — обход графа типов по определению рекурсивен
    std::uint32_t add(const Type& t) {
        if (!t)
            return kNoType;
        if (const auto it = index_.find(t.get()); it != index_.end())
            return it->second;

        // Сначала дети: их индексы должны быть меньше нашего.
        if (const auto* at = t->as<ArrayType>()) {
            add(at->elementType());
        }
        else if (const auto* st = t->as<StructType>()) {
            for (const StructMember& m : st->members())
                add(m.type);
        }
        else if (const auto* et = t->as<EnumType>()) {
            add(et->base());
        }

        const auto id = static_cast<std::uint32_t>(order_.size() + 1);
        order_.push_back(t);
        index_.emplace(t.get(), id);
        return id;
    }

    [[nodiscard]] std::uint32_t indexOf(const Type& t) const {
        if (!t)
            return kNoType;
        const auto it = index_.find(t.get());
        return it == index_.end() ? kNoType : it->second;
    }

    void write(ByteWriter& w) const {
        w.u32(static_cast<std::uint32_t>(order_.size()));
        for (const Type& t : order_)
            writeOne(w, t);
    }

private:
    void writeOne(ByteWriter& w, const Type& t) const {
        // Общая часть — только вид. fourState пишется лишь там, где он
        // действительно нужен при чтении: у производных видов конструктор
        // выводит его из детей, и записанное значение всё равно было бы
        // проигнорировано.
        w.u8(static_cast<std::uint8_t>(t->kind()));

        switch (t->kind()) {
            case TypeKind::STRING:
                break;
            case TypeKind::SCALAR:
                w.u8(static_cast<std::uint8_t>(t->fourState() ? 1 : 0));
                break;
            case TypeKind::REAL:
                w.u8(static_cast<std::uint8_t>(t->as<RealType>()->isShortreal() ? 1 : 0));
                break;
            case TypeKind::VECTOR: {
                const auto* vt = t->as<VectorType>();
                w.i32(vt->msb());
                w.i32(vt->lsb());
                w.u8(static_cast<std::uint8_t>(vt->isSigned() ? 1 : 0));
                w.u8(static_cast<std::uint8_t>(vt->fourState() ? 1 : 0));
                break;
            }
            case TypeKind::ARRAY: {
                const auto* at = t->as<ArrayType>();
                w.u32(indexOf(at->elementType()));
                w.i32(at->indexLeft());
                w.i32(at->indexRight());
                w.u8(static_cast<std::uint8_t>(at->packed() ? 1 : 0));
                break;
            }
            case TypeKind::STRUCT:
            case TypeKind::UNION: {
                const auto* st = t->as<StructType>();
                w.u8(static_cast<std::uint8_t>(st->packed() ? 1 : 0));
                w.u32(static_cast<std::uint32_t>(st->members().size()));
                for (const StructMember& m : st->members()) {
                    w.str(m.name);
                    w.u32(indexOf(m.type));
                    w.u32(m.bitOffset);
                }
                break;
            }
            case TypeKind::ENUM: {
                const auto* et = t->as<EnumType>();
                w.u32(indexOf(et->base()));
                w.u32(static_cast<std::uint32_t>(et->entries().size()));
                for (const EnumEntry& e : et->entries()) {
                    w.str(e.name);
                    w.u64(e.value);
                }
                break;
            }
            default:
                // EVENT/VOID: классов-дескрипторов у них нет, то есть такой Type
                // через публичный API не создать. Молчать тут нельзя — иначе
                // файл окажется недописанным.
                throw Exception{"hierarchy: type kind is not serializable"};
        }
    }

private:
    std::vector<Type> order_;
    std::unordered_map<const TypeDescriptor*, std::uint32_t> index_;
};

/// Прочитать таблицу типов. types[0] всегда пустой Type.
std::vector<Type> readTypes(ByteReader& r) {
    std::uint32_t count = 0;
    if (!r.u32(count))
        throw Exception{"hierarchy: truncated type table"};

    std::vector<Type> types;
    types.reserve(static_cast<std::size_t>(count) + 1);
    types.emplace_back();  // индекс 0 — «типа нет»

    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t rawKind = 0;
        if (!r.u8(rawKind))
            throw Exception{"hierarchy: truncated type"};

        // Ссылка обязана указывать на УЖЕ прочитанный тип: таблица записана
        // топологически, и это же условие ловит подделанные индексы.
        const auto resolve = [&types](std::uint32_t ref) -> const Type& {
            if (ref >= types.size())
                throw Exception{"hierarchy: forward or out-of-range type reference"};
            return types[ref];
        };

        switch (static_cast<TypeKind>(rawKind)) {
            case TypeKind::SCALAR: {
                std::uint8_t fourState = 0;
                if (!r.u8(fourState))
                    throw Exception{"hierarchy: truncated scalar type"};
                types.push_back(makeScalar(fourState != 0));
                break;
            }
            case TypeKind::STRING:
                types.push_back(makeString());
                break;
            case TypeKind::REAL: {
                std::uint8_t shortreal = 0;
                if (!r.u8(shortreal))
                    throw Exception{"hierarchy: truncated real type"};
                types.push_back(makeReal(shortreal != 0));
                break;
            }
            case TypeKind::VECTOR: {
                std::int32_t msb = 0;
                std::int32_t lsb = 0;
                std::uint8_t isSigned = 0;
                std::uint8_t fourState = 0;
                if (!r.i32(msb) || !r.i32(lsb) || !r.u8(isSigned) || !r.u8(fourState))
                    throw Exception{"hierarchy: truncated vector type"};
                types.push_back(makeVector(msb, lsb, isSigned != 0, fourState != 0));
                break;
            }
            case TypeKind::ARRAY: {
                std::uint32_t elem = 0;
                std::int32_t left = 0;
                std::int32_t right = 0;
                std::uint8_t packed = 0;
                if (!r.u32(elem) || !r.i32(left) || !r.i32(right) || !r.u8(packed))
                    throw Exception{"hierarchy: truncated array type"};
                types.push_back(makeArray(resolve(elem), left, right, packed != 0));
                break;
            }
            case TypeKind::STRUCT:
            case TypeKind::UNION: {
                std::uint8_t packed = 0;
                std::uint32_t members = 0;
                if (!r.u8(packed) || !r.u32(members))
                    throw Exception{"hierarchy: truncated struct type"};

                std::vector<StructMember> list;
                list.reserve(members);
                for (std::uint32_t m = 0; m < members; ++m) {
                    StructMember sm;
                    std::uint32_t ref = 0;
                    if (!r.str(sm.name) || !r.u32(ref) || !r.u32(sm.bitOffset))
                        throw Exception{"hierarchy: truncated struct member"};
                    sm.type = resolve(ref);
                    list.push_back(std::move(sm));
                }
                // makeStruct() union не строит, а makeUnion() в публичном API нет —
                // конструктор сам по себе публичный, зовём его.
                const bool isUnion = static_cast<TypeKind>(rawKind) == TypeKind::UNION;
                types.push_back(std::make_shared<const StructType>(std::move(list), packed != 0, isUnion));
                break;
            }
            case TypeKind::ENUM: {
                std::uint32_t base = 0;
                std::uint32_t entries = 0;
                if (!r.u32(base) || !r.u32(entries))
                    throw Exception{"hierarchy: truncated enum type"};

                std::vector<EnumEntry> list;
                list.reserve(entries);
                for (std::uint32_t e = 0; e < entries; ++e) {
                    EnumEntry entry;
                    if (!r.str(entry.name) || !r.u64(entry.value))
                        throw Exception{"hierarchy: truncated enum entry"};
                    list.push_back(std::move(entry));
                }
                types.push_back(std::make_shared<const EnumType>(resolve(base), std::move(list)));
                break;
            }
            default:
                throw Exception{"hierarchy: unknown type kind"};
        }
    }
    return types;
}

}  // namespace

void encodeHierarchy(ByteWriter& w, const Hierarchy& h) {
    TypeTable types;
    for (std::size_t i = 0; i < h.signalCount(); ++i)
        types.add(h.signalNode(NodeId{static_cast<NodeId::ValueType>(i)}).type);
    types.write(w);

    w.u32(static_cast<std::uint32_t>(h.scopeCount()));
    for (std::size_t i = 0; i < h.scopeCount(); ++i) {
        const auto& s = h.scopeNode(ScopeId{static_cast<ScopeId::ValueType>(i)});
        w.str(s.name);
        w.u8(static_cast<std::uint8_t>(s.kind));
        w.u32(s.parent.get());  // у корня это невалидный сентинел, он и запишется
    }

    w.u32(static_cast<std::uint32_t>(h.signalCount()));
    for (std::size_t i = 0; i < h.signalCount(); ++i) {
        const auto& n = h.signalNode(NodeId{static_cast<NodeId::ValueType>(i)});
        w.str(n.name);
        w.u32(types.indexOf(n.type));
        w.u32(n.scope.get());
        w.u32(n.parent.get());
        w.u32(n.stream.get());
        w.u8(static_cast<std::uint8_t>(n.projection ? 1 : 0));
        if (n.projection) {
            w.u32(n.projection->offset);
            w.u32(n.projection->width);
        }
    }
}

Hierarchy decodeHierarchy(ByteReader& r) {
    const std::vector<Type> types = readTypes(r);

    Hierarchy h;

    std::uint32_t scopeCount = 0;
    if (!r.u32(scopeCount) || scopeCount == 0)
        throw Exception{"hierarchy: truncated scope table"};

    for (std::uint32_t i = 0; i < scopeCount; ++i) {
        std::string name;
        std::uint8_t kind = 0;
        std::uint32_t parent = 0;
        if (!r.str(name) || !r.u8(kind) || !r.u32(parent))
            throw Exception{"hierarchy: truncated scope"};
        if (kind > static_cast<std::uint8_t>(ScopeKind::UNKNOWN))
            throw Exception{"hierarchy: unknown scope kind"};
        if (i == 0)
            continue;  // корень уже создан конструктором Hierarchy

        const ScopeId id = h.addScope(ScopeId{parent}, std::move(name), static_cast<ScopeKind>(kind));
        if (id.get() != i)
            throw Exception{"hierarchy: scope order broken"};
    }

    std::uint32_t nodeCount = 0;
    if (!r.u32(nodeCount))
        throw Exception{"hierarchy: truncated node table"};

    for (std::uint32_t i = 0; i < nodeCount; ++i) {
        std::string name;
        std::uint32_t typeRef = 0;
        std::uint32_t scope = 0;
        std::uint32_t parent = 0;
        std::uint32_t stream = 0;
        std::uint8_t hasProjection = 0;
        if (!r.str(name) || !r.u32(typeRef) || !r.u32(scope) || !r.u32(parent) || !r.u32(stream) ||
                !r.u8(hasProjection))
            throw Exception{"hierarchy: truncated node"};
        if (typeRef >= types.size())
            throw Exception{"hierarchy: out-of-range type reference"};

        std::optional<BitSlice> projection;
        if (hasProjection != 0) {
            BitSlice slice;
            if (!r.u32(slice.offset) || !r.u32(slice.width))
                throw Exception{"hierarchy: truncated projection"};
            projection = slice;
        }

        // Родитель отличает члена композита от сигнала верхнего уровня.
        // addElement здесь не нужен: это addMember с именем "[i]", а имя у нас
        // записано буквально.
        const NodeId parentId{parent};
        const NodeId id = parentId.valid()
                ? h.addMember(parentId, std::move(name), types[typeRef], SignalId{stream}, projection)
                : h.addSignal(ScopeId{scope}, std::move(name), types[typeRef], SignalId{stream});
        if (id.get() != i)
            throw Exception{"hierarchy: node order broken"};
    }
    return h;
}

}  // namespace WaSafe
