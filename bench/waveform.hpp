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

/// Параметры дизайна с широкой развёрткой массивов.
///
/// Меряет потолок МЕТАДАННЫХ, а не значений: узлов много, изменений мало.
/// Каждый элемент unpacked-массива — отдельный SignalNode со своим потоком
/// (BaseBuilder::buildChildren разворачивает массив поэлементно, виртуализации
/// нет), поэтому число узлов задаётся здесь напрямую, а не выводится из дампа.
struct MetaSpec {
    std::uint32_t nodes = 1'000'000;  ///< суммарное число элементов массивов
    std::uint32_t arrays = 4;         ///< на сколько массивов они разложены
    std::uint32_t changeShare = 64;   ///< изменения пишет каждый N-й элемент
    WaSafe::TimeStamp endTime = 64;   ///< длительность: SignalIndex должен быть непуст
    std::uint64_t seed = 2026'09'05;
};

/// Объявить дизайн с массивами и прогнать редкие изменения. finish() НЕ зовётся.
Stats generateMeta(WaSafe::Builder& sink, const MetaSpec& spec);

}  // namespace Bench
