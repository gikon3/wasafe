#pragma once

#include <cstdint>
#include <vector>

#include "wasafe/io/builder.hpp"

namespace Bench {

/// Параметры синтетического дампа. Смесь подобрана по тому, как выглядят
/// настоящие дампы: несколько быстрых сигналов дают львиную долю изменений,
/// а большинство шин меняется редко.
struct Spec {
    std::uint32_t streams = 1024;         ///< число листовых потоков
    WaSafe::TimeStamp endTime = 200'000;  ///< длительность в тиках
    std::uint64_t seed = 2026'08'26;      ///< детерминированность прогонов
    std::uint32_t signalsPerScope = 32;   ///< сигналов в одном модуле
};

struct Stats {
    std::uint64_t changes = 0;
    std::uint32_t streams = 0;
};

/// Объявить дизайн и прогнать поток изменений в приёмник. finish() НЕ зовётся:
/// им распоряжается вызывающий.
Stats generate(WaSafe::Builder& sink, const Spec& spec);

/// Оценка числа изменений без их порождения — чтобы заранее подобрать масштаб.
[[nodiscard]] std::uint64_t estimateChanges(const Spec& spec);

}  // namespace Bench
