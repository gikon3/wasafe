#include "wasafe/storage/storage.hpp"

#include <utility>
#include <vector>

#include "storage/merge_cursor.hpp"

namespace WaSafe {

std::unique_ptr<Cursor> Storage::openCursor(std::span<const SignalId> ids, TimeRange range) const {
    // Общая реализация поверх одиночных курсоров: подходит любому storage, в том
    // числе внешнему. Пустой список даёт слияние без подкурсоров — такой курсор
    // сразу возвращает false, отдельный EmptyCursor не нужен.
    std::vector<std::unique_ptr<Cursor>> subs;
    subs.reserve(ids.size());
    for (const SignalId id : ids)
        subs.push_back(openCursor(id, range));
    return std::make_unique<MergeCursor>(std::move(subs));
}

}  // namespace WaSafe
