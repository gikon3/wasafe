#include "storage/merge_cursor.hpp"

#include <algorithm>
#include <numeric>
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

void MergeCursor::start() {
    // Подкурсоры сразу ставятся на первое изменение: ключ кучи — время текущего
    // изменения, поэтому пустые в неё не попадают вовсе.
    heap_.reserve(subs_.size());
    for (std::uint32_t i = 0; i < subs_.size(); ++i) {
        if (subs_[i]->next())
            heap_.push_back(i);
    }
    std::ranges::make_heap(heap_, [this](std::uint32_t a, std::uint32_t b) { return later(a, b); });
}

std::uint32_t MergeCursor::sourceOf(std::uint32_t sub) const noexcept {
    const std::uint32_t src = sources_[sub];
    return src != kKeepSource ? src : subs_[sub]->current().source;
}

bool MergeCursor::later(std::uint32_t lhs, std::uint32_t rhs) const noexcept {
    const TimeStamp lt = subs_[lhs]->current().time;
    const TimeStamp rt = subs_[rhs]->current().time;
    // Вторая компонента ключа — не украшение: без неё pop_heap выбирал бы среди
    // равных времён произвольный подкурсор, и порядок выдачи зависел бы от
    // размера кучи. Совпадающие номера источников (в штатном употреблении их не
    // бывает) дают эквивалентность — для кучи это допустимо.
    return lt != rt ? lt > rt : sourceOf(lhs) > sourceOf(rhs);
}

void MergeCursor::advanceLast() {
    if (last_ == kNone)
        return;

    const auto idx = static_cast<std::uint32_t>(last_);
    last_ = kNone;
    if (!subs_[idx]->next())
        return;  // подкурсор исчерпан — в кучу не возвращается

    heap_.push_back(idx);
    std::ranges::push_heap(heap_, [this](std::uint32_t a, std::uint32_t b) { return later(a, b); });
}

std::size_t MergeCursor::takeMin() {
    if (heap_.empty())
        return kNone;
    std::ranges::pop_heap(heap_, [this](std::uint32_t a, std::uint32_t b) { return later(a, b); });
    const std::uint32_t idx = heap_.back();
    heap_.pop_back();
    return idx;
}

bool MergeCursor::next() {
    advanceLast();

    const std::size_t best = takeMin();
    if (best == kNone)
        return false;

    current_ = subs_[best]->current();
    if (const std::uint32_t src = sources_[best]; src != kKeepSource)
        current_.source = src;
    last_ = best;
    return true;
}

}  // namespace WaSafe
