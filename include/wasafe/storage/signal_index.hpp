#pragma once

#include <cstdint>
#include <filesystem>
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

/// Индекс одного потока: упорядоченный по времени список блоков.
/// Позволяет за O(log N) найти блоки, пересекающие запрошенный диапазон.
struct SignalLocator {
    std::vector<BlockRef> blocks;  ///< отсортированы по time.begin

    /// Диапазон индексов блоков, пересекающих range: [first, last).
    [[nodiscard]] WASAFE_API std::pair<std::size_t, std::size_t> blocksIn(TimeRange range) const noexcept;
};

/// Глобальный индекс хранилища: путь к данным каждого сигнала и геометрия блоков.
/// Может быть построен при первом открытии (для VCD) либо сериализован в сайдкар
/// (*.wsfidx), чтобы повторные открытия не сканировали файл целиком.
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

    // --- сериализация сайдкара ----------------------------------------------
    void save(const std::filesystem::path& path) const;
    [[nodiscard]] static SignalIndex load(const std::filesystem::path& path);

private:
    std::unordered_map<SignalId, SignalLocator> streams_;
    TimeRange timeRange_{};
    TimeScale timeScale_{};
};

}  // namespace WaSafe
