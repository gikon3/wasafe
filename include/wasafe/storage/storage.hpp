#pragma once

#include <memory>
#include <span>

#include "wasafe/core/ids.hpp"
#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"
#include "wasafe/storage/value_cursor.hpp"
#include "wasafe/types/value.hpp"

namespace WaSafe {

/// Абстрактное хранилище ЛИСТОВЫХ потоков изменений значений.
///
/// Это граница между «что хранится» и «откуда берётся». Реализации:
///   * MemoryStorage  — всё в ОЗУ (после полной загрузки/ingestion);
///   * LazyStorage — данные в файле + индекс по блокам; подгружает только
///     нужные блоки по запросу диапазона/сигнала (см. требование о динамической
///     подгрузке без полной загрузки в память).
///
/// Backend оперирует ТОЛЬКО листовыми SignalId. Сборка значений композитов
/// (struct/array) из листьев выполняется уровнем выше (Database),
/// поэтому интерфейс storage остаётся одинаковым для любого типа листа.
class WASAFE_API Storage {
public:
    virtual ~Storage() = default;

    // --- метаданные времени -------------------------------------------------
    [[nodiscard]] virtual TimeRange timeRange() const = 0;
    [[nodiscard]] virtual TimeScale timeScale() const = 0;

    // --- точечный доступ ----------------------------------------------------
    /// Значение потока в момент t (последнее изменение на или до t).
    /// Возвращаемый ValueView валиден до следующего обращения к storage в этом
    /// потоке исполнения (данные лежат во внутреннем scratch-буфере).
    [[nodiscard]] virtual ValueView valueAt(SignalId id, TimeStamp timestamp) const = 0;

    // --- потоковый доступ по диапазону (ленивый) ----------------------------
    /// Открыть курсор по изменениям потока в диапазоне. Реализация ленивого
    /// storage подгружает только пересекающиеся с range блоки.
    [[nodiscard]] virtual std::unique_ptr<Cursor> openCursor(SignalId id, TimeRange range) const = 0;

    /// Открыть курсор по изменениям НЕСКОЛЬКИХ потоков сразу, слитым в общий
    /// хронологический порядок. current().source — индекс потока в ids; при
    /// совпадении времени порядок выдачи следует порядку ids.
    ///
    /// Реализация по умолчанию сливает одиночные курсоры на куче. Файловому
    /// storage переопределение позволило бы пройти блоки всех потоков одним
    /// последовательным проходом; текущий LazyStorage этим не пользуется —
    /// его курсоры и так читают блоки по одному, минуя LRU.
    [[nodiscard]] virtual std::unique_ptr<Cursor> openCursor(std::span<const SignalId> ids, TimeRange range) const;

    // --- навигация по фронтам -----------------------------------------------
    [[nodiscard]] virtual TimeStamp nextChange(SignalId id, TimeStamp after) const = 0;
    [[nodiscard]] virtual TimeStamp prevChange(SignalId id, TimeStamp before) const = 0;

    // --- управление памятью (no-op для in-memory) ---------------------------
    /// Подсказка: вероятно, скоро понадобятся эти потоки в этом диапазоне.
    virtual void prefetch(std::span<const SignalId> /*ids*/, TimeRange /*range*/) {}
    /// Подсказка: освободить кэш блоков вне диапазона.
    virtual void release(TimeRange /*keep*/) {}
    /// Текущий объём кэша блоков в байтах (0 — если неприменимо).
    [[nodiscard]] virtual std::size_t cachedBytes() const { return 0; }

protected:
    Storage() = default;
    Storage(const Storage&) = default;
    Storage(Storage&&) = default;

    Storage& operator=(const Storage&) = default;
    Storage& operator=(Storage&&) = default;
};

}  // namespace WaSafe
