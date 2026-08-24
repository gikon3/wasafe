#include "wasafe/storage/lazy_storage.hpp"

#include <algorithm>
#include <functional>
#include <list>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "wasafe/storage/decoded_block.hpp"

namespace WaSafe {

namespace {

// --- индексные помощники по упорядоченным массивам --------------------------

/// Индекс последнего блока с time.begin <= t, либо SIZE_MAX.
std::size_t blockAtOrBefore(const std::vector<BlockRef>& bs, TimeStamp t) noexcept {
    const auto it = std::ranges::upper_bound(bs, t, std::less{}, [](const BlockRef& b) { return b.time.begin; });
    return it == bs.begin() ? SIZE_MAX : static_cast<std::size_t>((it - 1) - bs.begin());
}

/// Индекс первого блока с time.end > t (т.е. способного содержать изменения после t).
std::size_t firstBlockEndingAfter(const std::vector<BlockRef>& bs, TimeStamp t) noexcept {
    const auto it = std::ranges::lower_bound(bs, t, std::less_equal{}, [](const BlockRef& b) { return b.time.end; });
    return static_cast<std::size_t>(it - bs.begin());
}

/// Индекс последнего блока с time.begin < t, либо SIZE_MAX.
std::size_t lastBlockBeginningBefore(const std::vector<BlockRef>& bs, TimeStamp t) noexcept {
    const auto it = std::ranges::lower_bound(bs, t, std::less{}, [](const BlockRef& b) { return b.time.begin; });
    return it == bs.begin() ? SIZE_MAX : static_cast<std::size_t>((it - 1) - bs.begin());
}

/// Индекс последнего изменения с times[i] <= t, либо SIZE_MAX.
std::size_t changeAtOrBefore(const TimeColumn& times, TimeStamp t) noexcept {
    const std::size_t u = times.upperBound(t);
    return u == 0 ? SIZE_MAX : u - 1;
}

/// Курсор «конца данных»: всегда пуст (нет потока / пустой диапазон).
class EmptyCursor final : public Cursor {
public:
    [[nodiscard]] bool next() override { return false; }
    [[nodiscard]] const ValueChange& current() const noexcept override { return cur_; }

private:
    ValueChange cur_{};
};

/// Ленивый курсор по диапазону: загружает по одному блоку из источника, минуя
/// общий LRU-кэш (последовательное сканирование не должно вытеснять кэш точечных
/// обращений). current().value указывает внутрь текущего блока курсора и валиден
/// до следующего next(), пересекающего границу блока.
class LazyCursor final : public Cursor {
public:
    LazyCursor(const BlockSource* src, const SignalLocator* loc, TimeRange range, std::size_t blockFirst,
            std::size_t blockLast) : src_(src), loc_(loc), range_(range), block_(blockFirst), blockLast_(blockLast) {}

    [[nodiscard]] bool next() override {
        for (;;) {
            if (!loaded_) {
                if (block_ >= blockLast_)
                    return false;
                load(block_);
                // первый блок может начинаться раньше range.begin — пропускаем хвост
                pos_ = cur_.times().lowerBound(range_.begin);
                loaded_ = true;
            }

            const TimeColumn& times = cur_.times();
            if (pos_ < times.size()) {
                const TimeStamp t = times[pos_];
                if (t >= range_.end)
                    return false;  // дальше — только больше
                current_.time = t;
                current_.value = cur_.valueAtIndex(pos_);
                ++pos_;
                return true;
            }

            // блок исчерпан — к следующему (его первое изменение уже >= range.begin)
            if (++block_ >= blockLast_)
                return false;
            load(block_);
            pos_ = 0;
        }
    }

    [[nodiscard]] const ValueChange& current() const noexcept override { return current_; }

private:
    void load(std::size_t bi) { cur_ = decodeBlock(src_->readBlock(loc_->blocks[bi])); }

    const BlockSource* src_;
    const SignalLocator* loc_;
    TimeRange range_;
    std::size_t block_;
    std::size_t blockLast_;
    DecodedBlock cur_;
    std::size_t pos_ = 0;
    bool loaded_ = false;
    ValueChange current_{};
};

}  // namespace

// ---------------------------------------------------------------------------
// LRU-кэш декодированных блоков (ключ — смещение блока в источнике).
// ---------------------------------------------------------------------------
class LazyStorage::BlockCache {
public:
    explicit BlockCache(std::size_t limit) : limit_(limit) {}

    /// Вернуть декодированный блок, при промахе загрузив через источник.
    /// Ссылка стабильна, пока блок не вытеснен (т.е. до следующего обращения,
    /// способного спровоцировать вытеснение).
    const DecodedBlock& get(const BlockRef& ref, const BlockSource& src) {
        if (const auto it = map_.find(ref.offset); it != map_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second);  // переместить в начало (MRU)
            return it->second->block;
        }
        lru_.push_front(Entry{ref, decodeBlock(src.readBlock(ref))});
        map_[ref.offset] = lru_.begin();
        bytes_ += lru_.front().block.byteSize();
        evict();
        return lru_.front().block;
    }

    void release(TimeRange keep) {
        for (auto it = lru_.begin(); it != lru_.end();) {
            if (!it->ref.time.overlaps(keep)) {
                bytes_ -= it->block.byteSize();
                map_.erase(it->ref.offset);
                it = lru_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

private:
    struct Entry {
        BlockRef ref;
        DecodedBlock block;
    };

    void evict() {
        // никогда не вытесняем единственный (только что добавленный) блок
        while (bytes_ > limit_ && lru_.size() > 1) {
            Entry const& back = lru_.back();
            bytes_ -= back.block.byteSize();
            map_.erase(back.ref.offset);
            lru_.pop_back();
        }
    }

    std::size_t limit_;
    std::size_t bytes_ = 0;
    std::list<Entry> lru_;  // front = MRU
    std::unordered_map<std::uint64_t, std::list<Entry>::iterator> map_;
};

// ---------------------------------------------------------------------------
// LazyStorage
// ---------------------------------------------------------------------------
LazyStorage::LazyStorage(SignalIndex index, std::unique_ptr<BlockSource> source, Options opts) :
        index_(std::move(index)), source_(std::move(source)), cache_(std::make_unique<BlockCache>(opts.cacheBytes)),
        opts_(opts) {
}

// Здесь BlockCache полон — можно генерировать уничтожение unique_ptr<BlockCache>.
LazyStorage::~LazyStorage() = default;

TimeRange LazyStorage::timeRange() const {
    return index_.timeRange();
}
TimeScale LazyStorage::timeScale() const {
    return index_.timeScale();
}

ValueView LazyStorage::valueAt(SignalId id, TimeStamp t) const {
    const SignalLocator* loc = index_.locate(id);
    if (!loc || loc->blocks.empty())
        return {};

    const std::size_t bi = blockAtOrBefore(loc->blocks, t);
    if (bi == SIZE_MAX)
        return {};  // t раньше начала всех данных

    const DecodedBlock& blk = cache_->get(loc->blocks[bi], *source_);
    if (const std::size_t idx = changeAtOrBefore(blk.times(), t); idx != SIZE_MAX) {
        return blk.valueAtIndex(idx);
    }
    // t < первого изменения этого блока — значение перенесено из предыдущего блока
    if (bi == 0)
        return {};
    const DecodedBlock& prev = cache_->get(loc->blocks[bi - 1], *source_);
    return prev.empty() ? ValueView{} : prev.valueAtIndex(prev.count() - 1);
}

std::unique_ptr<Cursor> LazyStorage::openCursor(SignalId id, TimeRange range) const {
    const SignalLocator* loc = index_.locate(id);
    if (!loc || range.empty())
        return std::make_unique<EmptyCursor>();
    const auto [first, last] = loc->blocksIn(range);
    if (first >= last)
        return std::make_unique<EmptyCursor>();
    return std::make_unique<LazyCursor>(source_.get(), loc, range, first, last);
}

TimeStamp LazyStorage::nextChange(SignalId id, TimeStamp after) const {
    const SignalLocator* loc = index_.locate(id);
    if (!loc || loc->blocks.empty())
        return kNoTime;

    for (std::size_t bi = firstBlockEndingAfter(loc->blocks, after); bi < loc->blocks.size(); ++bi) {
        const DecodedBlock& blk = cache_->get(loc->blocks[bi], *source_);
        const TimeColumn& times = blk.times();
        if (const std::size_t i = times.upperBound(after); i != times.size())
            return times[i];
        // в этом блоке нет изменений > after; первое изменение следующего блока подойдёт
    }
    return kNoTime;
}

TimeStamp LazyStorage::prevChange(SignalId id, TimeStamp before) const {
    const SignalLocator* loc = index_.locate(id);
    if (!loc || loc->blocks.empty())
        return kNoTime;

    const std::size_t start = lastBlockBeginningBefore(loc->blocks, before);
    if (start == SIZE_MAX)
        return kNoTime;

    for (std::size_t bi = start + 1; bi-- > 0;) {
        const DecodedBlock& blk = cache_->get(loc->blocks[bi], *source_);
        const TimeColumn& times = blk.times();
        if (const std::size_t i = times.lowerBound(before); i != 0)
            return times[i - 1];
        // все изменения этого блока >= before; ищем в предыдущем
    }
    return kNoTime;
}

void LazyStorage::prefetch(std::span<const SignalId> ids, TimeRange range) {
    for (const SignalId id : ids) {
        const SignalLocator* loc = index_.locate(id);
        if (!loc)
            continue;
        const auto [first, last] = loc->blocksIn(range);
        for (std::size_t bi = first; bi < last; ++bi) {
            (void)cache_->get(loc->blocks[bi], *source_);
        }
    }
}

void LazyStorage::release(TimeRange keep) {
    cache_->release(keep);
}

std::size_t LazyStorage::cachedBytes() const {
    return cache_->bytes();
}

}  // namespace WaSafe
