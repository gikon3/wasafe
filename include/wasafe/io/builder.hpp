#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "wasafe/core/ids.hpp"
#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"
#include "wasafe/model/scope.hpp"
#include "wasafe/types/type.hpp"
#include "wasafe/types/value.hpp"

namespace WaSafe {

class Database;

/// ЕДИНЫЙ интерфейс записи диаграмм из любого источника.
///
/// Любой парсер формата (VCD, FST, собственный бинарный, генератор «на лету»)
/// наполняет хранилище ТОЛЬКО через этот приёмник, ничего не зная о том, как
/// данные будут храниться. Это и есть «общий интерфейс для записи из любого
/// формата».
///
/// Протокол двухфазный:
///   1. Заголовок: set_time_scale, begin_scope/end_scope, declare_var, header_done.
///   2. Значения:  чередование set_time и value_change; завершается finish().
///
/// Композитные типы объявляются одним declare_var с композитным Type. Реализация
/// сама решает, разворачивать ли packed-структуру в битовые срезы одного потока
/// или завести отдельные потоки на члены — наружу это не протекает.
class WASAFE_API Builder {
public:
    virtual ~Builder() = default;

    // --- фаза заголовка -----------------------------------------------------
    virtual void setTimeScale(TimeScale scale) = 0;
    /// Войти в новый scope (относительно текущего). Возвращает его id.
    virtual ScopeId beginScope(std::string_view name, ScopeKind kind) = 0;
    virtual void endScope() = 0;

    /// Объявить переменную в текущем scope.
    ///
    /// @param alias   если задан — переменная разделяет поток уже объявленного
    ///                сигнала (VCD-алиасы: один код на несколько имён).
    /// @param leaves  явная привязка потоков, выделяемых при РАЗВОРАЧИВАНИИ детей
    ///                композита. Нужна форматам, где элементы композита приходят
    ///                самостоятельными потоками (VCD: mem[0], mem[1], ... — каждый
    ///                со своим кодом).
    ///
    ///                Пустой span — потоки выделяет ядро, а их id выводятся из
    ///                порядка разворачивания. Иначе размер обязан равняться
    ///                expansionStreamCount(type), а порядок — порядку
    ///                разворачивания: глубина-первым, элементы массива по
    ///                ArrayType::ordinalOf (ord = 0 — это indexLeft, а НЕ меньший
    ///                индекс: для [3:0] первым идёт [3]), члены структуры — в
    ///                порядке объявления. Валидный id на входе привязывает элемент
    ///                к УЖЕ существующему потоку (алиас на уровне элемента),
    ///                невалидный — выделяет новый и записывается на месте.
    ///
    ///                Для типа, представимого ОДНИМ потоком (лист или
    ///                packed-композит), span обязан быть пустым: его поток
    ///                задаётся параметром alias. Вид и ширина потока, переданного
    ///                в leaves, НЕ проверяются на совпадение с типом элемента —
    ///                ровно как и у alias.
    /// @return SignalId листового потока, в который пойдут value_change;
    ///         у unpacked-композита невалиден.
    [[nodiscard]] virtual SignalId declareVar(std::string_view name, Type type,
            std::optional<SignalId> alias = std::nullopt, std::span<SignalId> leaves = {}) = 0;

    /// Конец фазы заголовка; после этого иерархия зафиксирована.
    virtual void headerDone() = 0;

    // --- фаза значений ------------------------------------------------------
    /// Установить текущую метку времени (монотонно неубывающую).
    virtual void setTime(TimeStamp time) = 0;
    /// Записать изменение значения потока в текущий момент времени.
    virtual void valueChange(SignalId id, ValueView value) = 0;

    /// Завершить запись: уплотнить/проиндексировать данные.
    virtual void finish() = 0;

    /// Извлечь готовую БД. Вызывается один раз после finish().
    [[nodiscard]] virtual Database takeDatabase() = 0;

protected:
    Builder() = default;
    Builder(const Builder&) = default;
    Builder(Builder&&) = default;

    Builder& operator=(const Builder&) = default;
    Builder& operator=(Builder&&) = default;
};

/// Builder, собирающий БД целиком в ОЗУ (поверх MemoryStorage).
/// Подходит для небольших/средних дампов и тестов.
[[nodiscard]] WASAFE_API std::unique_ptr<Builder> makeMemoryBuilder();

/// Настройки индексирующего builder'а.
struct WASAFE_API IndexingOptions {
    /// Максимум изменений в одном блоке (0 — значение по умолчанию, 4096).
    /// Меньшие блоки = мельче ленивая подгрузка при чтении.
    std::size_t blockChanges = 0;
    /// Верхний предел ОЗУ под накопители блоков (0 — значение по умолчанию,
    /// 64 МиБ). При его превышении незаполненные накопители сбрасываются
    /// досрочно, поэтому пиковая память не зависит ни от размера дампа, ни от
    /// числа сигналов — ценой более мелких блоков и чуть большего индекса.
    std::size_t bufferBytes = 0;
};

/// Builder, пишущий нормализованное блочное хранилище (*.wsfstore) и индекс
/// (*.wsfidx) на диск. Позволяет затем открыть результат ленивым storage и
/// читать его без полной загрузки в память — независимо от исходного формата.
///
/// Запись потоковая: блок уходит на диск, как только накопил blockChanges
/// изменений (или раньше — под давлением bufferBytes), поэтому вся диаграмма
/// в ОЗУ не собирается. Следствие: блоки разных потоков чередуются в файле, и
/// store создаётся/усекается уже в конструкторе — прерванная ingestion
/// оставляет частичный файл.
[[nodiscard]] WASAFE_API std::unique_ptr<Builder> makeIndexingBuilder(const std::filesystem::path& storePath,
        IndexingOptions opts = {});

}  // namespace WaSafe
