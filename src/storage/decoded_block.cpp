#include "wasafe/storage/decoded_block.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "core/byte_io.hpp"
#include "storage/bit_planes.hpp"

namespace WaSafe {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

// ---------------------------------------------------------------------------
// DecodedBlock
// ---------------------------------------------------------------------------
DecodedBlock::DecodedBlock(ValueKind kind, std::uint32_t width) {
    switch (kind) {
        case ValueKind::LOGIC:
            values_ = LogicStore{width, {}, {}};
            break;
        case ValueKind::REAL:
            values_ = RealStore{};
            break;
        case ValueKind::STRING:
            values_ = StringStore{{}, {0}};  // ведущее смещение арены
            break;
        default:
            break;  // std::monostate — блок без значений
    }
}

void DecodedBlock::append(TimeStamp t, ValueView v) {
    times_.append(t);
    // clang-format off
    std::visit(
            Overloaded{
                [](std::monostate&) {},
                [&](LogicStore& s) { appendLogicValue(s.width, s.aval, s.bval, v.logic()); },
                [&](RealStore& s) { s.values.push_back(v.real()); },
                [&](StringStore& s) {
                    const std::string_view str = v.string();
                    s.arena.insert(s.arena.end(), str.begin(), str.end());
                    s.offsets.push_back(static_cast<std::uint32_t>(s.arena.size()));
                },
            },
            values_);
    // clang-format on
}

DecodedBlock DecodedBlock::slice(std::size_t lo, std::size_t hi) const {
    DecodedBlock b;
    b.times_ = times_.slice(lo, hi);
    // clang-format off
    std::visit(
            Overloaded{
                [](const std::monostate&) {},
                [&](const LogicStore& s) {
                    const std::size_t wp = LogicVectorView::wordsFor(s.width);
                    LogicStore out;
                    out.width = s.width;
                    out.aval.assign(s.aval.begin() + lo * wp, s.aval.begin() + hi * wp);
                    if (!s.bval.empty())
                        out.bval.assign(s.bval.begin() + lo * wp, s.bval.begin() + hi * wp);
                    b.values_ = std::move(out);
                },
                [&](const RealStore& s) {
                    b.values_ = RealStore{{s.values.begin() + lo, s.values.begin() + hi}};
                },
                [&](const StringStore& s) {
                    const std::uint32_t a = s.offsets[lo];
                    const std::uint32_t z = s.offsets[hi];
                    StringStore out;
                    out.arena.assign(s.arena.begin() + a, s.arena.begin() + z);
                    out.offsets.reserve(hi - lo + 1);
                    for (std::size_t k = lo; k <= hi; ++k)
                        out.offsets.push_back(s.offsets[k] - a);
                    b.values_ = std::move(out);
                },
            },
            values_);
    // clang-format on
    return b;
}

// NOLINTNEXTLINE(bugprone-exception-escape) — variant никогда не valueless: move-конструкторы всех альтернатив noexcept
ColumnView DecodedBlock::columns() const noexcept {
    // clang-format off
    return std::visit(
            Overloaded{
                [](const std::monostate&) { return ColumnView{}; },
                [](const LogicStore& s) { return ColumnView{s.width, s.aval, s.bval}; },
                [](const RealStore& s) { return ColumnView{s.values}; },
                [](const StringStore& s) { return ColumnView{s.arena, s.offsets}; },
            },
            values_);
    // clang-format on
}

ValueView DecodedBlock::valueAtIndex(std::size_t i) const noexcept {
    return i < count() ? columns().valueAt(i) : ValueView{};
}

// NOLINTNEXTLINE(bugprone-exception-escape) — variant никогда не valueless: move-конструкторы всех альтернатив noexcept
std::size_t DecodedBlock::byteSize() const noexcept {
    const std::size_t valuesBytes = std::visit(
            Overloaded{
                    [](const std::monostate&) -> std::size_t { return 0; },
                    [](const LogicStore& s) { return (s.aval.size() + s.bval.size()) * sizeof(std::uint64_t); },
                    [](const RealStore& s) { return s.values.size() * sizeof(double); },
                    [](const StringStore& s) {
                        return s.arena.size() * sizeof(char) + s.offsets.size() * sizeof(std::uint32_t);
                    },
            },
            values_);
    return sizeof(DecodedBlock) + times_.byteSize() + valuesBytes;
}

// NOLINTNEXTLINE(bugprone-exception-escape) — variant никогда не valueless: move-конструкторы всех альтернатив noexcept
ValueKind DecodedBlock::kind() const noexcept {
    // clang-format off
    return std::visit(
            Overloaded{
                [](const std::monostate&) { return ValueKind::NONE; },
                [](const LogicStore&) { return ValueKind::LOGIC; },
                [](const RealStore&) { return ValueKind::REAL; },
                [](const StringStore&) { return ValueKind::STRING; },
            },
            values_);
    // clang-format on
}

std::uint32_t DecodedBlock::width() const noexcept {
    const auto* logic = std::get_if<LogicStore>(&values_);
    return logic ? logic->width : 0u;
}

// ---------------------------------------------------------------------------
// Сериализация (little-endian, версия 3)
// ---------------------------------------------------------------------------
namespace {

constexpr std::uint32_t kBlockMagic = 0x33424457u;  // 'WDB3'

/// Флаги блока (байт flags заголовка).
constexpr std::uint8_t kFlagHasBval = 1u << 0;  ///< bval-план присутствует (блок четырёхзначный)

}  // namespace

std::vector<std::byte> encodeBlock(const DecodedBlock& block) {
    std::vector<std::byte> out;
    ByteWriter w{out};

    const auto* logic = std::get_if<DecodedBlock::LogicStore>(&block.values_);

    // Заголовок пишется ПО ПОЛЯМ: раскладка структуры и её выравнивание не
    // должны протекать в файл.
    w.u32(kBlockMagic);
    w.u8(static_cast<std::uint8_t>(block.kind()));
    w.u8((logic != nullptr && !logic->bval.empty()) ? kFlagHasBval : std::uint8_t{0});
    w.u32(block.width());
    w.u32(static_cast<std::uint32_t>(block.count()));

    // Время: первая метка зигзагом, дальше беззнаковые дельты. Метки идут
    // неубывающе и обычно плотно, поэтому восемь байт на изменение
    // превращаются в один-два.
    const std::size_t n = block.times_.size();
    if (n != 0) {
        w.svarint(block.times_[0]);
        for (std::size_t i = 1; i < n; ++i)
            w.varint(static_cast<std::uint64_t>(block.times_[i]) - static_cast<std::uint64_t>(block.times_[i - 1]));
    }

    // clang-format off
    std::visit(
            Overloaded{
                [](const std::monostate&) {},
                [&](const DecodedBlock::LogicStore& s) {
                    w.array(std::span{s.aval});
                    w.array(std::span{s.bval});  // пуст у двухзначного блока
                },
                [&](const DecodedBlock::RealStore& s) { w.array(std::span{s.values}); },
                [&](const DecodedBlock::StringStore& s) {
                    // off[count+1], затем длина арены и сама арена.
                    w.array(std::span{s.offsets});
                    w.u32(static_cast<std::uint32_t>(s.arena.size()));
                    w.bytes(std::as_bytes(std::span{s.arena}));
                },
            },
            block.values_);
    // clang-format on
    return out;
}

// Плоский декодер формата: одна ветка на вид записи, внутри «прочитать поля,
// проверить обрыв». Метрика штрафует switch с такими проверками, но разнесение
// по функциям на один вызов каждая спрятало бы раскладку формата, а не
// упростило её.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
DecodedBlock decodeBlock(std::span<const std::byte> raw) {
    ByteReader r{raw};

    std::uint32_t magic = 0;
    std::uint8_t kind = 0;
    std::uint8_t flags = 0;
    std::uint32_t width = 0;
    std::uint32_t count = 0;
    if (!r.u32(magic) || !r.u8(kind) || !r.u8(flags) || !r.u32(width) || !r.u32(count))
        throw Exception{"block: truncated header"};
    if (magic != kBlockMagic)
        throw Exception{"block: bad magic"};

    DecodedBlock b;
    TimeStamp prev = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (i == 0) {
            std::int64_t first = 0;
            if (!r.svarint(first))
                throw Exception{"block: truncated times"};
            prev = first;
        }
        else {
            std::uint64_t delta = 0;
            if (!r.varint(delta))
                throw Exception{"block: truncated times"};
            prev = static_cast<TimeStamp>(static_cast<std::uint64_t>(prev) + delta);
        }
        b.times_.append(prev);  // колонка сама выберет узкое/широкое представление
    }

    switch (static_cast<ValueKind>(kind)) {
        case ValueKind::LOGIC: {
            const std::size_t words = LogicVectorView::wordsFor(width);
            const std::size_t n = static_cast<std::size_t>(count) * words;
            DecodedBlock::LogicStore s;
            s.width = width;
            s.aval.resize(n);
            if (!r.array(std::span{s.aval}))
                throw Exception{"block: truncated logic"};
            if ((flags & kFlagHasBval) != 0) {
                s.bval.resize(n);
                if (!r.array(std::span{s.bval}))
                    throw Exception{"block: truncated logic"};
            }
            b.values_ = std::move(s);
            break;
        }
        case ValueKind::REAL: {
            DecodedBlock::RealStore s;
            s.values.resize(count);
            if (!r.array(std::span{s.values}))
                throw Exception{"block: truncated reals"};
            b.values_ = std::move(s);
            break;
        }
        case ValueKind::STRING: {
            DecodedBlock::StringStore s;
            s.offsets.resize(static_cast<std::size_t>(count) + 1);
            if (!r.array(std::span{s.offsets}))
                throw Exception{"block: truncated string offsets"};
            std::uint32_t arena = 0;
            if (!r.u32(arena))
                throw Exception{"block: truncated arena size"};
            s.arena.resize(arena);
            if (!r.bytes(std::as_writable_bytes(std::span{s.arena})))
                throw Exception{"block: truncated string arena"};
            b.values_ = std::move(s);
            break;
        }
        default:
            break;
    }
    return b;
}

}  // namespace WaSafe
