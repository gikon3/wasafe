#include "wasafe/storage/memory_storage.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <variant>

namespace WaSafe {

namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

/// Курсор поверх плотных массивов одного потока в ОЗУ. ColumnView/спан времён
/// указывают внутрь хранилища потока (стабильны, пока жив storage), поэтому
/// ValueView из current() валиден сколь угодно долго; интерфейс Cursor лишь
/// требует валидности до next().
class MemoryCursor final : public Cursor {
public:
    /// Стартовая позиция — первое РЕАЛЬНОЕ изменение в диапазоне: значение
    /// «переноса» на range.begin курсор не синтезирует (для снимка есть valueAt).
    MemoryCursor(ColumnView cols, const TimeColumn& times, TimeRange range) :
            cols_{cols}, times_{&times}, range_{range}, pos_{times.lowerBound(range.begin)} {}

    [[nodiscard]] bool next() override {
        if (pos_ >= times_->size())
            return false;
        const TimeStamp t = (*times_)[pos_];
        if (t >= range_.end)
            return false;
        current_.time = t;
        current_.value = cols_.valueAt(pos_);
        ++pos_;
        return true;
    }

    [[nodiscard]] const ValueChange& current() const noexcept override { return current_; }

private:
    ColumnView cols_;
    const TimeColumn* times_;
    TimeRange range_;
    std::size_t pos_ = 0;
    ValueChange current_{};
};

/// Пустой курсор (нет потока / пустой диапазон).
class EmptyCursor final : public Cursor {
public:
    [[nodiscard]] bool next() override { return false; }
    [[nodiscard]] const ValueChange& current() const noexcept override { return cur_; }

private:
    ValueChange cur_{};
};

/// Сентинел «индекс не найден».
constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

/// Индекс последнего изменения с times[i] <= t, либо kNone.
std::size_t changeAtOrBefore(const TimeColumn& times, TimeStamp t) noexcept {
    const std::size_t u = times.upperBound(t);
    return u == 0 ? kNone : u - 1;
}

}  // namespace

MemoryStorage::Stream::Stream(ValueKind kind, std::uint32_t width) {
    switch (kind) {
        case ValueKind::LOGIC:
            values_ = LogicStore{width, {}, {}};  // планы наполняются в append
            break;
        case ValueKind::REAL:
            values_ = RealStore{};
            break;
        case ValueKind::STRING:
            values_ = StringStore{{}, {0}};  // ведущее смещение арены
            break;
        default:
            break;  // std::monostate — поток без значений
    }
}

void MemoryStorage::Stream::append(TimeStamp t, ValueView v) {
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
                    // bval заводится лишь когда он реально нужен; первое
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

// NOLINTNEXTLINE(bugprone-exception-escape) — variant никогда не valueless: move-конструкторы всех альтернатив noexcept
ColumnView MemoryStorage::Stream::columns() const noexcept {
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

const MemoryStorage::Stream* MemoryStorage::stream(SignalId id) const noexcept {
    return id.valid() && id.get() < streams_.size() ? &streams_[id.get()] : nullptr;
}

void MemoryStorage::reserveStreams(std::size_t n) {
    streams_.reserve(n);
}

SignalId MemoryStorage::createStream(ValueKind kind, std::uint32_t width) {
    const auto id = SignalId{static_cast<SignalId::ValueType>(streams_.size())};
    streams_.emplace_back(kind, width);
    return id;
}

void MemoryStorage::append(SignalId id, TimeStamp t, ValueView v) {
    if (!id.valid() || id.get() >= streams_.size())
        return;
    streams_[id.get()].append(t, v);
}

void MemoryStorage::finalize() {
    // Общий диапазон времени: [min первого изменения, max последнего + 1).
    // Полуоткрытый конец «+1» делает последний фронт содержащимся в диапазоне.
    std::optional<std::pair<TimeStamp, TimeStamp>> span;
    for (const Stream& s : streams_) {
        if (s.empty())
            continue;
        const auto times = s.times();
        const TimeStamp f = times.front();
        const TimeStamp l = times.back();
        if (span) {
            span->first = std::min(span->first, f);
            span->second = std::max(span->second, l);
        }
        else {
            span = {f, l};
        }
    }
    timeRange_ = span ? TimeRange{span->first, span->second + 1} : TimeRange{};
}

ValueView MemoryStorage::valueAt(SignalId id, TimeStamp timestamp) const {
    const Stream* s = stream(id);
    if (!s || s->empty())
        return {};
    const std::size_t i = changeAtOrBefore(s->times(), timestamp);
    if (i == kNone)
        return {};  // timestamp раньше первого изменения потока
    return s->columns().valueAt(i);
}

std::unique_ptr<Cursor> MemoryStorage::openCursor(SignalId id, TimeRange range) const {
    const Stream* s = stream(id);
    if (!s || range.empty())
        return std::make_unique<EmptyCursor>();
    return std::make_unique<MemoryCursor>(s->columns(), s->times(), range);
}

TimeStamp MemoryStorage::nextChange(SignalId id, TimeStamp after) const {
    const Stream* s = stream(id);
    if (!s)
        return kNoTime;
    const TimeColumn& times = s->times();
    const std::size_t i = times.upperBound(after);
    return i == times.size() ? kNoTime : times[i];
}

TimeStamp MemoryStorage::prevChange(SignalId id, TimeStamp before) const {
    const Stream* s = stream(id);
    if (!s)
        return kNoTime;
    const TimeColumn& times = s->times();
    const std::size_t i = times.lowerBound(before);
    return i == 0 ? kNoTime : times[i - 1];
}

}  // namespace WaSafe
