#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wasafe/core/exception.hpp"
#include "wasafe/core/ids.hpp"
#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"
#include "wasafe/storage/block_source.hpp"

namespace WaSafe {

class ByteReader;
class ByteWriter;

/// Индекс одного потока: упорядоченный по времени список блоков.
/// Позволяет за O(log N) найти блоки, пересекающие запрошенный диапазон.
struct SignalLocator {
    std::vector<BlockRef> blocks;  ///< отсортированы по time.begin

    /// Диапазон индексов блоков, пересекающих range: [first, last).
    [[nodiscard]] WASAFE_API std::pair<std::size_t, std::size_t> blocksIn(TimeRange range) const noexcept;
};

/// Глобальный индекс хранилища: путь к данным каждого сигнала и геометрия блоков.
/// Строится при разборе источника и уезжает в хвост *.wsfstore, чтобы повторные
/// открытия не сканировали исходный дамп заново.
class WASAFE_API SignalIndex {
public:
    SignalIndex() = default;

    [[nodiscard]] TimeRange timeRange() const noexcept { return timeRange_; }
    [[nodiscard]] TimeScale timeScale() const noexcept { return timeScale_; }
    void setTimeRange(TimeRange r) noexcept { timeRange_ = r; }
    void setTimeScale(TimeScale s) noexcept { timeScale_ = s; }

    [[nodiscard]] const SignalLocator* locate(SignalId id) const noexcept;
    SignalLocator& locator(SignalId id) { return streams_[id]; }
    void addBlock(SignalId id, BlockRef block);

    [[nodiscard]] std::size_t streamCount() const noexcept { return streams_.size(); }

    /// Во что обходится индекс в ОЗУ.
    /// Проход по всем локаторам, то есть O(N потоков).
    [[nodiscard]] std::size_t byteSize() const noexcept;

    // --- сериализация ---------------------------------------------------------
    // Индекс пишется секцией в общий поток файла store, а не отдельным файлом:
    // без него значения бесполезны, и разделять их было бы нечего. ByteWriter и
    // ByteReader — внутренние типы, поэтому позвать это можно только из ядра.
    void encode(ByteWriter& w) const;
    [[nodiscard]] static SignalIndex decode(ByteReader& r);

private:
    std::unordered_map<SignalId, SignalLocator> streams_;
    TimeRange timeRange_{};
    TimeScale timeScale_{};
};

}  // namespace WaSafe
