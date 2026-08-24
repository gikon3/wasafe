#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "wasafe/export.hpp"
#include "wasafe/storage/block_source.hpp"
#include "wasafe/storage/signal_index.hpp"
#include "wasafe/storage/storage.hpp"

namespace WaSafe {

struct WASAFE_API LazyStorageOptions {
    std::size_t cacheBytes = 256ull << 20;  ///< верхний предел кэша блоков
};

/// Ленивый storage поверх файла: держит в памяти только индекс и небольшой
/// LRU-кэш распакованных блоков. Значения подгружаются по запросу диапазона/
/// сигнала, поэтому открытие гигабайтного дампа не загружает его в ОЗУ.
///
/// Это реализация требования: «обращение по временным диапазонам или имени
/// сигнала в файл, чтобы подгружать диаграммы динамически без полной загрузки».
class WASAFE_API LazyStorage final : public Storage {
public:
    using Options = LazyStorageOptions;

public:
    LazyStorage(SignalIndex index, std::unique_ptr<BlockSource> source, Options opts = {});
    LazyStorage(const LazyStorage&) = delete;
    LazyStorage(LazyStorage&&) = delete;
    ~LazyStorage() override;

    // Storage
    [[nodiscard]] TimeRange timeRange() const override;
    [[nodiscard]] TimeScale timeScale() const override;
    [[nodiscard]] ValueView valueAt(SignalId id, TimeStamp t) const override;
    [[nodiscard]] std::unique_ptr<Cursor> openCursor(SignalId id, TimeRange range) const override;
    [[nodiscard]] TimeStamp nextChange(SignalId id, TimeStamp after) const override;
    [[nodiscard]] TimeStamp prevChange(SignalId id, TimeStamp before) const override;

    // Пакетная перегрузка openCursor(span) живёт в базовом классе: без using
    // объявление override одиночной скрыло бы её при поиске имени.
    using Storage::openCursor;

    void prefetch(std::span<const SignalId> ids, TimeRange range) override;
    void release(TimeRange keep) override;
    [[nodiscard]] std::size_t cachedBytes() const override;

    [[nodiscard]] const SignalIndex& index() const noexcept { return index_; }

    LazyStorage& operator=(const LazyStorage&) = delete;
    LazyStorage& operator=(LazyStorage&&) = delete;

private:
    class BlockCache;  ///< LRU декодированных блоков (pimpl)

private:
    SignalIndex index_;
    std::unique_ptr<BlockSource> source_;
    std::unique_ptr<BlockCache> cache_;
    Options opts_;
};

}  // namespace WaSafe
