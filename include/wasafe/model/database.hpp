#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "wasafe/core/exception.hpp"
#include "wasafe/core/ids.hpp"
#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"
#include "wasafe/model/hierarchy.hpp"
#include "wasafe/model/scope.hpp"
#include "wasafe/model/signal.hpp"
#include "wasafe/storage/storage.hpp"
#include "wasafe/storage/value_cursor.hpp"

namespace WaSafe {

/// Высокоуровневое хранилище временных диаграмм.
///
/// Объединяет иерархию (структура + типы) и storage (значения). Предоставляет
/// поиск по имени/пути и времени, возвращая единый хэндл Signal, через который
/// одинаково доступны скаляры, шины, структуры и многомерные массивы.
///
/// БД не умеет открывать файлы форматов сама и ничего о них не знает: парсер
/// (Reader) выбирает пользователь и наполняет им Builder — см. io/ingest.hpp.
/// Режим (всё в ОЗУ либо ленивое чтение из нормализованного store) определяется
/// выбранным builder'ом, а не скрытым флагом.
///
/// ПОТОКИ. БД после сборки только читает, но потокобезопасной НЕ является
/// (почему именно — см. storage.hpp): один экземпляр рассчитан на один поток
/// исполнения. Второму потоку нужен свой экземпляр поверх тех же данных.
/// При этом NodeId и SignalId между экземплярами переносимы — иерархия у них
/// общая, — а хэндлы Signal и Scope нет: каждый держит указатель на конкретную
/// Database, и использовать его можно только с ней.
class WASAFE_API Database final {
public:
    /// Собрать БД из уже построенной иерархии и готового storage (после ingestion
    /// в память или поверх собственного нормализованного хранилища).
    Database(Hierarchy hierarchy, std::unique_ptr<Storage> storage);
    Database(const Database&) = delete;
    Database(Database&&) = default;
    ~Database() = default;

    // --- метаданные ---------------------------------------------------------
    /// Иерархия живёт, пока жива хотя бы одна БД, её разделяющая, — то есть
    /// ссылка переживает и уничтожение ЭТОГО экземпляра, если остались дубликаты.
    /// Бросает у перемещённой БД: разыменовывать нулевой указатель нельзя, а
    /// noexcept это исключило бы (та же развилка, что у Signal::database()).
    [[nodiscard]] const Hierarchy& hierarchy() const {
        if (!hierarchy_)
            throwMovedFrom();
        return *hierarchy_;
    }
    [[nodiscard]] const Storage& storage() const noexcept { return *storage_; }
    [[nodiscard]] Storage& storage() noexcept { return *storage_; }
    [[nodiscard]] TimeRange timeRange() const { return storage_->timeRange(); }
    [[nodiscard]] TimeScale timeScale() const { return storage_->timeScale(); }

    // --- навигация по иерархии ----------------------------------------------
    [[nodiscard]] Scope root() const;
    /// Найти сигнал по полному пути ("top.u_cpu.regs[3].valid"). Работает для
    /// любого уровня вложенности, включая элементы массивов и члены структур.
    [[nodiscard]] std::optional<Signal> find(std::string_view path) const;
    /// Найти scope по пути.
    [[nodiscard]] std::optional<Scope> findScope(std::string_view path) const;

    // --- доступ к значениям на уровне узла (за этим стоят методы Signal) ----
    [[nodiscard]] Value valueAt(NodeId id, TimeStamp timestamp) const;
    /// Изменения одного узла. Для композита без собственного потока листовые
    /// изменения сливаются по времени, и current().source — индекс листа в
    /// leafNodes(id); во всех остальных случаях source равен 0.
    [[nodiscard]] ValueCursor changes(NodeId id, TimeRange range) const;
    /// Изменения сразу нескольких узлов, слитые в общий хронологический порядок:
    /// один проход вместо N независимых курсоров. current().source — индекс узла
    /// в nodes (внутренняя разбивка композитов наружу не видна); при совпадении
    /// времени порядок выдачи следует порядку nodes.
    ///
    /// Индекс, а не SignalId: алиасы VCD дают несколько узлов на один поток, а
    /// packed-члены одного вектора — одну и ту же проекцию.
    [[nodiscard]] ValueCursor changes(std::span<const NodeId> nodes, TimeRange range) const;
    [[nodiscard]] TimeStamp nextChange(NodeId id, TimeStamp after) const;
    [[nodiscard]] TimeStamp prevChange(NodeId id, TimeStamp before) const;

    // --- перечисление листьев (вход для пакетного чтения и экспорта) --------
    /// Листовые узлы поддерева, обходом в глубину. Узел с собственным потоком —
    /// сам лист: packed-структура наружу отдаётся одним потоком.
    [[nodiscard]] std::vector<NodeId> leafNodes(NodeId id) const;
    /// То же для поддерева scope, рекурсивно по вложенным scope. Вход для
    /// экспортёра: leafNodes(root()) + changes(...) дают все изменения дизайна
    /// в общем хронологическом порядке.
    [[nodiscard]] std::vector<NodeId> leafNodes(ScopeId scope) const;

    // фабрики хэндлов
    [[nodiscard]] Signal signalHandle(NodeId id) const;
    [[nodiscard]] Scope scopeHandle(ScopeId id) const;

    // --- размножение по потокам ---------------------------------------------
    /// Независимая равноценная БД поверх ТЕХ ЖЕ данных — по одной на поток.
    /// Иерархия и значения разделяются, а изменяемое (кэш блоков, дескриптор
    /// файла) у дубликата своё, поэтому потоки не мешают друг другу и ничего не
    /// синхронизируют. Дубликат не зависит от оригинала и переживает его.
    ///
    /// Это не копирующий конструктор: операция открывает файл заново, то есть
    /// стоит дорого и может бросить. Дубликат заводят на поток (или на окно), а
    /// не на запрос — LRU у него начинается пустым.
    ///
    /// Бросает Exception, если storage размножения не поддерживает: у формата со
    /// своим блочным устройством может не быть BlockSource::duplicate().
    [[nodiscard]] Database duplicate() const;

    Database& operator=(const Database&) = delete;
    Database& operator=(Database&&) = default;

private:
    /// Поверх УЖЕ разделяемой иерархии — для duplicate(). Приватный намеренно:
    /// то, что иерархия лежит за shared_ptr, остаётся деталью реализации, иначе
    /// сменить стратегию владения стало бы ломающим изменением.
    Database(std::shared_ptr<const Hierarchy> hierarchy, std::unique_ptr<Storage> storage);

    /// Бросок вынесен из inline-аксессора в холодную функцию.
    [[noreturn]] static void throwMovedFrom();

    /// Собрать листовые потоки поддерева узла (для merge-курсора композита).
    void collectLeafStreams(NodeId id, std::vector<SignalId>& out) const;

    /// Рекурсивные накопители для leafNodes().
    void collectLeafNodes(NodeId id, std::vector<NodeId>& out) const;
    void collectLeafNodes(ScopeId scope, std::vector<NodeId>& out) const;

    /// Тело changes(NodeId, TimeRange) без обёртки в ValueCursor: слиянию нужен
    /// сам unique_ptr<Cursor>, а разобрать ValueCursor обратно нельзя.
    [[nodiscard]] std::unique_ptr<Cursor> openNodeCursor(NodeId id, TimeRange range) const;

    /// Ближайший предок узла, владеющий потоком (для packed-члена — носитель
    /// его битов). Невалидный NodeId, если такого нет.
    [[nodiscard]] NodeId owningAncestor(NodeId id) const;

    /// Срез потока в момент времени. nullopt — у потока нет значения на этот
    /// момент либо оно не logic.
    [[nodiscard]] std::optional<LogicVector> sliceAt(SignalId stream, BitSlice slice, TimeStamp t) const;

private:
    /// Неизменяема после сборки, поэтому дубликаты её разделяют.
    std::shared_ptr<const Hierarchy> hierarchy_;
    /// А вот storage свой у каждого: в нём и живёт изменяемое состояние.
    std::unique_ptr<Storage> storage_;
};

}  // namespace WaSafe
