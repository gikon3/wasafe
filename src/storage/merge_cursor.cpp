#include "storage/merge_cursor.hpp"

#include <algorithm>
#include <numeric>
#include <optional>
#include <utility>

namespace WaSafe {

namespace {

/// Нумерация источников по порядку подкурсоров: 0, 1, 2, ...
std::vector<std::uint32_t> identitySources(std::size_t count) {
    std::vector<std::uint32_t> ids(count);
    std::ranges::iota(ids, std::uint32_t{0});
    return ids;
}

}  // namespace

// Порядок инициализации членов — по объявлению (subs_ раньше sources_), поэтому
// subs_.size() здесь уже осмыслен; делегирующий конструктор не годится: порядок
// вычисления его аргументов не определён, и size() читался бы у перемещённого.
MergeCursor::MergeCursor(std::vector<std::unique_ptr<Cursor>> subs) :
        subs_{std::move(subs)}, sources_{identitySources(subs_.size())} {
    start();
}

MergeCursor::MergeCursor(std::vector<std::unique_ptr<Cursor>> subs, std::vector<std::uint32_t> sources) :
        subs_{std::move(subs)}, sources_{std::move(sources)} {
    start();
}

bool MergeCursor::later(const Key& lhs, const Key& rhs) noexcept {
    // Вторая компонента ключа — не украшение: без неё pop_heap выбирал бы среди
    // равных времён произвольный подкурсор, и порядок выдачи зависел бы от
    // размера кучи. Совпадающие номера источников (в штатном употреблении их не
    // бывает) дают эквивалентность — для кучи это допустимо.
    return lhs.time != rhs.time ? lhs.time > rhs.time : lhs.source > rhs.source;
}

void MergeCursor::start() {
    // Подкурсоры сразу ставятся на первое изменение: ключ кучи — время текущего
    // изменения, поэтому пустые в неё не попадают вовсе.
    heap_.reserve(subs_.size());
    for (std::uint32_t i = 0; i < subs_.size(); ++i) {
        if (subs_[i]->next())
            heap_.push_back(keyOf(i));
    }
    std::ranges::make_heap(heap_, later);
}

MergeCursor::Key MergeCursor::keyOf(std::uint32_t sub) const noexcept {
    return Key{.time = subs_[sub]->current().time, .source = sourceOf(sub), .sub = sub};
}

std::uint32_t MergeCursor::sourceOf(std::uint32_t sub) const noexcept {
    const std::uint32_t src = sources_[sub];
    return src != kKeepSource ? src : subs_[sub]->current().source;
}

void MergeCursor::advanceLast() {
    if (last_ == kNone)
        return;

    const auto idx = static_cast<std::uint32_t>(last_);
    last_ = kNone;
    if (!subs_[idx]->next())
        return;  // подкурсор исчерпан — в кучу не возвращается

    heap_.push_back(keyOf(idx));
    std::ranges::push_heap(heap_, later);
}

std::optional<MergeCursor::Key> MergeCursor::takeMin() {
    if (heap_.empty())
        return std::nullopt;
    std::ranges::pop_heap(heap_, later);
    const Key key = heap_.back();
    heap_.pop_back();
    return key;
}

bool MergeCursor::next() {
    advanceLast();

    const std::optional<Key> best = takeMin();
    if (!best)
        return false;

    current_ = subs_[best->sub]->current();
    current_.source = best->source;  // разрешён при заталкивании в кучу
    last_ = best->sub;
    return true;
}

}  // namespace WaSafe
