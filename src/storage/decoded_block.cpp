#include "wasafe/storage/decoded_block.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>
#include <variant>

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
                [&](LogicStore& s) {
                    const std::uint32_t words = LogicVectorView::wordsFor(s.width);
                    const std::size_t base = s.aval.size();
                    s.aval.resize(base + words, 0);

                    const LogicVectorView src = v.logic();
                    // bval заводится лишь когда он реально нужен. Первое
                    // четырёхзначное значение разворачивает план нулями под уже
                    // записанные изменения — они были двухзначными, нули верны.
                    if (!s.bval.empty() || !src.isTwoState())
                        s.bval.resize(base + words, 0);

                    for (std::uint32_t bit = 0; bit < s.width; ++bit) {
                        const Logic g = src[bit];
                        const std::size_t word = bit / 64u;
                        const std::uint64_t mask = std::uint64_t{1} << (bit % 64u);
                        if (logicA(g))
                            s.aval[base + word] |= mask;
                        if (logicB(g))
                            s.bval[base + word] |= mask;
                    }
                },
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
// Сериализация (хостовый порядок байт, версия 1)
// ---------------------------------------------------------------------------
namespace {

constexpr std::uint32_t kBlockMagic = 0x32424457u;  // 'WDB2'

/// Флаги блока (байт flags заголовка).
constexpr std::uint8_t kFlagHasBval = 1u << 0;  ///< bval-план присутствует (блок четырёхзначный)

struct Header {
    std::uint32_t magic;
    std::uint8_t kind;
    std::uint8_t flags;
    std::array<std::uint8_t, 2> pad;
    std::uint32_t width;
    std::uint32_t count;
};

// --- LEB128 ------------------------------------------------------------------
// Метки времени внутри блока идут неубывающе и обычно плотно, поэтому первая
// пишется зигзагом (на случай отрицательной), а остальные — беззнаковыми
// дельтами. Восемь байт на изменение превращаются в один-два.

void putVarint(std::vector<std::byte>& out, std::uint64_t v) {
    while (v >= 0x80u) {
        out.push_back(static_cast<std::byte>((v & 0x7Fu) | 0x80u));
        v >>= 7u;
    }
    out.push_back(static_cast<std::byte>(v));
}

[[nodiscard]] std::uint64_t zigzag(TimeStamp v) noexcept {
    return (static_cast<std::uint64_t>(v) << 1u) ^ static_cast<std::uint64_t>(v >> 63);
}

[[nodiscard]] TimeStamp unzigzag(std::uint64_t v) noexcept {
    return static_cast<TimeStamp>((v >> 1u) ^ (~(v & 1u) + 1u));
}

/// Дописать сырые байты POD-объекта в конец буфера.
template <class T>
void put(std::vector<std::byte>& out, const T& v) {
    const std::size_t off = out.size();
    out.resize(off + sizeof(T));
    std::memcpy(out.data() + off, &v, sizeof(T));
}
template <class T>
void putN(std::vector<std::byte>& out, const T* data, std::size_t n) {
    if (n == 0)
        return;
    const std::size_t off = out.size();
    out.resize(off + (n * sizeof(T)));
    std::memcpy(out.data() + off, data, n * sizeof(T));
}

/// Курсор чтения с проверкой границ.
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}

    template <class T>
    [[nodiscard]] bool read(T& out) noexcept {
        if (pos_ + sizeof(T) > data_.size())
            return false;
        std::memcpy(&out, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }
    template <class T>
    [[nodiscard]] bool readN(T* out, std::size_t n) noexcept {
        if (n == 0)
            return true;
        if (pos_ + n * sizeof(T) > data_.size())
            return false;
        std::memcpy(out, data_.data() + pos_, n * sizeof(T));
        pos_ += n * sizeof(T);
        return true;
    }

    [[nodiscard]] bool readVarint(std::uint64_t& out) noexcept {
        out = 0;
        for (unsigned shift = 0; shift < 64u; shift += 7u) {
            if (pos_ >= data_.size())
                return false;
            const auto byte = static_cast<std::uint8_t>(data_[pos_++]);
            out |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0)
                return true;
        }
        return false;  // больше десяти байт — повреждённый поток
    }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

}  // namespace

std::vector<std::byte> encodeBlock(const DecodedBlock& block) {
    std::vector<std::byte> out;

    const auto* logic = std::get_if<DecodedBlock::LogicStore>(&block.values_);

    Header h{};
    h.magic = kBlockMagic;
    h.kind = static_cast<std::uint8_t>(block.kind());
    h.flags = (logic != nullptr && !logic->bval.empty()) ? kFlagHasBval : std::uint8_t{0};
    h.width = block.width();
    h.count = static_cast<std::uint32_t>(block.count());
    put(out, h);

    // Время: первая метка зигзагом, дальше беззнаковые дельты.
    const std::size_t n = block.times_.size();
    if (n != 0) {
        putVarint(out, zigzag(block.times_[0]));
        for (std::size_t i = 1; i < n; ++i)
            putVarint(out,
                    static_cast<std::uint64_t>(block.times_[i]) - static_cast<std::uint64_t>(block.times_[i - 1]));
    }

    // clang-format off
    std::visit(
            Overloaded{
                [](const std::monostate&) {},
                [&](const DecodedBlock::LogicStore& s) {
                    putN(out, s.aval.data(), s.aval.size());
                    putN(out, s.bval.data(), s.bval.size());  // пуст у двухзначного блока
                },
                [&](const DecodedBlock::RealStore& s) { putN(out, s.values.data(), s.values.size()); },
                [&](const DecodedBlock::StringStore& s) {
                    // off[count+1], затем длина арены и сама арена.
                    putN(out, s.offsets.data(), s.offsets.size());
                    const std::uint32_t arena = static_cast<std::uint32_t>(s.arena.size());
                    put(out, arena);
                    putN(out, s.arena.data(), s.arena.size());
                },
            },
            block.values_);
    // clang-format on
    return out;
}

DecodedBlock decodeBlock(std::span<const std::byte> raw) {
    Reader r(raw);
    Header h{};
    if (!r.read(h))
        throw Exception{"block: truncated header"};
    if (h.magic != kBlockMagic)
        throw Exception{"block: bad magic"};

    DecodedBlock b;
    TimeStamp prev = 0;
    for (std::uint32_t i = 0; i < h.count; ++i) {
        std::uint64_t raw = 0;
        if (!r.readVarint(raw))
            throw Exception{"block: truncated times"};
        prev = (i == 0) ? unzigzag(raw) : static_cast<TimeStamp>(static_cast<std::uint64_t>(prev) + raw);
        b.times_.append(prev);  // колонка сама выберет узкое/широкое представление
    }

    switch (static_cast<ValueKind>(h.kind)) {
        case ValueKind::LOGIC: {
            const std::size_t words = LogicVectorView::wordsFor(h.width);
            const std::size_t n = static_cast<std::size_t>(h.count) * words;
            DecodedBlock::LogicStore s;
            s.width = h.width;
            s.aval.resize(n);
            if (!r.readN(s.aval.data(), n))
                throw Exception{"block: truncated logic"};
            if ((h.flags & kFlagHasBval) != 0) {
                s.bval.resize(n);
                if (!r.readN(s.bval.data(), n))
                    throw Exception{"block: truncated logic"};
            }
            b.values_ = std::move(s);
            break;
        }
        case ValueKind::REAL: {
            DecodedBlock::RealStore s;
            s.values.resize(h.count);
            if (!r.readN(s.values.data(), h.count))
                throw Exception{"block: truncated reals"};
            b.values_ = std::move(s);
            break;
        }
        case ValueKind::STRING: {
            DecodedBlock::StringStore s;
            s.offsets.resize(static_cast<std::size_t>(h.count) + 1);
            if (!r.readN(s.offsets.data(), s.offsets.size()))
                throw Exception{"block: truncated string offsets"};
            std::uint32_t arena = 0;
            if (!r.read(arena))
                throw Exception{"block: truncated arena size"};
            s.arena.resize(arena);
            if (!r.readN(s.arena.data(), arena))
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
