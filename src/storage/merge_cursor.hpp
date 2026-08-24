#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "wasafe/storage/value_cursor.hpp"

namespace WaSafe {

/// Курсор слияния нескольких подкурсоров по времени: отдаёт их изменения в общем
/// хронологическом порядке, помечая каждое номером источника.
///
/// Выбор ближайшего изменения идёт по мин-куче индексов подкурсоров, поэтому
/// стоимость шага — O(log N), а не O(N). Корректен для любого storage: выбранный
/// подкурсор не продвигается, пока его значение не будет прочитано потребителем
/// (продвижение — в начале следующего next()), ведь ValueView подкурсора живёт
/// лишь до его собственного next().
///
/// При совпадении времени порядок выдачи следует НОМЕРАМ ИСТОЧНИКОВ: ключ кучи —
/// пара (время, номер источника). Номер, а не индекс подкурсора: подкурсоры могут
/// идти в ином порядке, чем источники (мультипотоковый подкурсор с kKeepSource
/// покрывает сразу несколько номеров), а обещание наружу дано именно про порядок
/// запрошенных источников. Без второй компоненты pop_heap был бы недетерминирован.
class MergeCursor final : public Cursor {
public:
    /// Не переписывать source: подкурсор проставил его сам.
    static constexpr std::uint32_t kKeepSource = std::numeric_limits<std::uint32_t>::max();

public:
    /// Источники нумеруются по порядку подкурсоров.
    explicit MergeCursor(std::vector<std::unique_ptr<Cursor>> subs);
    /// sources[i] — что писать в current().source для i-го подкурсора, либо
    /// kKeepSource, если подкурсор сам мультипотоковый и уже проставил номер.
    MergeCursor(std::vector<std::unique_ptr<Cursor>> subs, std::vector<std::uint32_t> sources);

    [[nodiscard]] bool next() override;
    [[nodiscard]] const ValueChange& current() const noexcept override { return current_; }

private:
    /// «Нет выбранного подкурсора» (в last_).
    static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

private:
    /// Поставить все подкурсоры на первое изменение и построить кучу.
    void start();
    /// Продвинуть подкурсор, чьё значение уже отдали, и вернуть его в кучу.
    void advanceLast();
    /// Извлечь из кучи индекс подкурсора с ближайшим изменением; kNone — пусто.
    [[nodiscard]] std::size_t takeMin();
    /// Номер источника текущего изменения подкурсора: свой из sources_, либо —
    /// у мультипотокового подкурсора — тот, что он проставил сам.
    [[nodiscard]] std::uint32_t sourceOf(std::uint32_t sub) const noexcept;
    /// Строгий порядок «дальше» для мин-кучи на std::*_heap (те работают с
    /// макс-кучей, поэтому сравнение обратное).
    [[nodiscard]] bool later(std::uint32_t lhs, std::uint32_t rhs) const noexcept;

private:
    std::vector<std::unique_ptr<Cursor>> subs_;
    std::vector<std::uint32_t> sources_;
    std::vector<std::uint32_t> heap_;  ///< индексы живых подкурсоров, мин-куча
    std::size_t last_ = kNone;
    ValueChange current_{};
};

}  // namespace WaSafe
