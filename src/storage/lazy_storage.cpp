#include "wasafe/storage/lazy_storage.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>

#include "wasafe/storage/decoded_block.hpp"

namespace WaSafe {

namespace {

// --- индексные помощники по упорядоченным массивам --------------------------

/// Сентинел «индекс не найден».
constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

/// Индекс последнего блока с time.begin <= t, либо kNone.
std::size_t blockAtOrBefore(const std::vector<BlockRef>& bs, TimeStamp t) noexcept {
    const auto it = std::ranges::upper_bound(bs, t, std::less{}, [](const BlockRef& b) { return b.time.begin; });
    return it == bs.begin() ? kNone : std::distance(bs.begin(), std::prev(it));
}

/// Индекс первого блока с time.end > t (т.е. способного содержать изменения после t).
std::size_t firstBlockEndingAfter(const std::vector<BlockRef>& bs, TimeStamp t) noexcept {
    const auto it = std::ranges::lower_bound(bs, t, std::less_equal{}, [](const BlockRef& b) { return b.time.end; });
    return std::distance(bs.begin(), it);
}

/// Индекс последнего блока с time.begin < t, либо kNone.
std::size_t lastBlockBeginningBefore(const std::vector<BlockRef>& bs, TimeStamp t) noexcept {
    const auto it = std::ranges::lower_bound(bs, t, std::less{}, [](const BlockRef& b) { return b.time.begin; });
    return it == bs.begin() ? kNone : std::distance(bs.begin(), std::prev(it));
}

/// Индекс последнего изменения с times[i] <= t, либо kNone.
std::size_t changeAtOrBefore(const TimeColumn& times, TimeStamp t) noexcept {
    const std::size_t u = times.upperBound(t);
    return u == 0 ? kNone : u - 1;
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
    LazyCursor(const BlockSource* src, SignalId id, const SignalLocator* loc, TimeRange range, std::size_t blockFirst,
            std::size_t blockLast) :
            src_{src}, id_{id}, loc_{loc}, range_{range}, block_{blockFirst}, blockLast_{blockLast} {}

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
    void load(std::size_t bi) { cur_ = src_->decode(id_, loc_->blocks[bi]); }

    const BlockSource* src_;
    SignalId id_;
    const SignalLocator* loc_;
    TimeRange range_;
    std::size_t block_;
    std::size_t blockLast_;
    DecodedBlock cur_;
    std::size_t pos_ = 0;
    bool loaded_ = false;
    ValueChange current_{};
};

/// Ключ кэша блоков. Одного смещения мало: в чужом формате один чанк несёт
/// блоки нескольких потоков по общему offset, и они затирали бы друг друга.
/// Поток снимает это столкновение, cookie — случай, когда общее смещение делят
/// два блока ОДНОГО потока.
struct BlockKey {
    bool operator==(const BlockKey&) const noexcept = default;

    SignalId stream;
    std::uint64_t offset = 0;
    std::uint64_t cookie = 0;
};

struct BlockKeyHash {
    [[nodiscard]] std::size_t operator()(const BlockKey& k) const noexcept {
        std::size_t h = std::hash<SignalId>{}(k.stream);
        for (const std::uint64_t part : {k.offset, k.cookie})
            h ^= std::hash<std::uint64_t>{}(part) + 0x9e37'79b9'7f4a'7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// LRU-кэш декодированных блоков (ключ — поток плюс смещение блока).
// ---------------------------------------------------------------------------
class LazyStorage::BlockCache {
public:
    explicit BlockCache(std::size_t limit) : limit_{limit} {}

    /// Вернуть декодированный блок, при промахе загрузив через источник.
    /// Ссылка стабильна, пока блок не вытеснен (т.е. до следующего обращения,
    /// способного спровоцировать вытеснение).
    const DecodedBlock& get(SignalId id, const BlockRef& ref, const BlockSource& src) {
        const BlockKey key{.stream = id, .offset = ref.offset, .cookie = ref.cookie};
        if (const auto it = map_.find(key); it != map_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second);  // переместить в начало (MRU)
            return it->second->block;
        }
        lru_.push_front(Entry{key, ref.time, src.decode(id, ref)});
        map_[key] = lru_.begin();
        bytes_ += lru_.front().block.byteSize();
        evict();
        return lru_.front().block;
    }

    void release(TimeRange keep) {
        for (auto it = lru_.begin(); it != lru_.end();) {
            if (!it->time.overlaps(keep)) {
                bytes_ -= it->block.byteSize();
                map_.erase(it->key);
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
        BlockKey key;
        TimeRange time;  ///< покрытие блока: по нему работает release(keep)
        DecodedBlock block;
    };

    void evict() {
        // никогда не вытесняем единственный (только что добавленный) блок
        while (bytes_ > limit_ && lru_.size() > 1) {
            Entry const& back = lru_.back();
            bytes_ -= back.block.byteSize();
            map_.erase(back.key);
            lru_.pop_back();
        }
    }

    std::size_t limit_;
    std::size_t bytes_ = 0;
    std::list<Entry> lru_;  // front = MRU
    std::unordered_map<BlockKey, std::list<Entry>::iterator, BlockKeyHash> map_;
};

// ---------------------------------------------------------------------------
// LazyStorage
// ---------------------------------------------------------------------------
LazyStorage::LazyStorage(SignalIndex index, std::unique_ptr<BlockSource> source, Options opts) :
        LazyStorage{std::make_shared<const SignalIndex>(std::move(index)), std::move(source), opts} {
}

LazyStorage::LazyStorage(std::shared_ptr<const SignalIndex> index, std::unique_ptr<BlockSource> source, Options opts) :
        index_{std::move(index)}, source_{std::move(source)}, cache_{std::make_unique<BlockCache>(opts.cacheBytes)},
        opts_{opts} {
}

// Здесь BlockCache полон — можно генерировать уничтожение unique_ptr<BlockCache>.
LazyStorage::~LazyStorage() = default;

TimeRange LazyStorage::timeRange() const {
    return index_->timeRange();
}
TimeScale LazyStorage::timeScale() const {
    return index_->timeScale();
}

ValueView LazyStorage::valueAt(SignalId id, TimeStamp t) const {
    const SignalLocator* loc = index_->locate(id);
    if (!loc || loc->blocks.empty())
        return {};

    const std::size_t bi = blockAtOrBefore(loc->blocks, t);
    if (bi == kNone)
        return {};  // t раньше начала всех данных

    const DecodedBlock& blk = cache_->get(id, loc->blocks[bi], *source_);
    if (const std::size_t idx = changeAtOrBefore(blk.times(), t); idx != kNone) {
        return blk.valueAtIndex(idx);
    }
    // t < первого изменения этого блока — значение перенесено из предыдущего блока
    if (bi == 0)
        return {};
    const DecodedBlock& prev = cache_->get(id, loc->blocks[bi - 1], *source_);
    return prev.empty() ? ValueView{} : prev.valueAtIndex(prev.count() - 1);
}

std::unique_ptr<Cursor> LazyStorage::openCursor(SignalId id, TimeRange range) const {
    const SignalLocator* loc = index_->locate(id);
    if (!loc || range.empty())
        return std::make_unique<EmptyCursor>();
    const auto [first, last] = loc->blocksIn(range);
    if (first >= last)
        return std::make_unique<EmptyCursor>();
    return std::make_unique<LazyCursor>(source_.get(), id, loc, range, first, last);
}

TimeStamp LazyStorage::nextChange(SignalId id, TimeStamp after) const {
    const SignalLocator* loc = index_->locate(id);
    if (!loc || loc->blocks.empty())
        return kNoTime;

    for (std::size_t bi = firstBlockEndingAfter(loc->blocks, after); bi < loc->blocks.size(); ++bi) {
        const DecodedBlock& blk = cache_->get(id, loc->blocks[bi], *source_);
        const TimeColumn& times = blk.times();
        if (const std::size_t i = times.upperBound(after); i != times.size())
            return times[i];
        // в этом блоке нет изменений > after; первое изменение следующего блока подойдёт
    }
    return kNoTime;
}

TimeStamp LazyStorage::prevChange(SignalId id, TimeStamp before) const {
    const SignalLocator* loc = index_->locate(id);
    if (!loc || loc->blocks.empty())
        return kNoTime;

    const std::size_t start = lastBlockBeginningBefore(loc->blocks, before);
    if (start == kNone)
        return kNoTime;

    for (std::size_t bi = start + 1; bi-- > 0;) {
        const DecodedBlock& blk = cache_->get(id, loc->blocks[bi], *source_);
        const TimeColumn& times = blk.times();
        if (const std::size_t i = times.lowerBound(before); i != 0)
            return times[i - 1];
        // все изменения этого блока >= before; ищем в предыдущем
    }
    return kNoTime;
}

void LazyStorage::prefetch(std::span<const SignalId> ids, TimeRange range) {
    for (const SignalId id : ids) {
        const SignalLocator* loc = index_->locate(id);
        if (!loc)
            continue;
        const auto [first, last] = loc->blocksIn(range);
        for (std::size_t bi = first; bi < last; ++bi)
            std::ignore = cache_->get(id, loc->blocks[bi], *source_);
    }
}

void LazyStorage::release(TimeRange keep) {
    cache_->release(keep);
}

std::size_t LazyStorage::cachedBytes() const {
    return cache_->bytes();
}

std::size_t LazyStorage::metadataBytes() const {
    return index_->byteSize();
}

std::unique_ptr<Storage> LazyStorage::duplicate() const {
    auto source = source_->duplicate();
    if (!source)
        return nullptr;  // источник размножения не поддерживает — решает вызывающий
    // Индекс разделяется, кэш у дубликата свой и начинается пустым.
    return std::make_unique<LazyStorage>(index_, std::move(source), opts_);
}

}  // namespace WaSafe
