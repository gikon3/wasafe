#pragma once

#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include "wasafe/core/ids.hpp"
#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"
#include "wasafe/storage/value_cursor.hpp"
#include "wasafe/types/type.hpp"
#include "wasafe/types/value.hpp"

namespace WaSafe {

class Database;
class SignalChildRange;

/// Единый невладеющий дескриптор сигнала.
///
/// Ключевая идея библиотеки: ОДИН и тот же интерфейс применяется к чему угодно —
/// одиночному биту, многоразрядной шине, структуре, объединению, элементу
/// многомерного массива и любому вложенному полю. Композит (struct/array)
/// раскрывается через children()/child()/operator[], возвращающие снова Signal.
/// Доступ к значениям (value_at/changes) единообразен для листа и для композита:
/// для композита значение собирается из листьев «на лету».
///
/// Хэндл лёгкий (указатель на БД + индекс узла); копируется свободно и валиден,
/// пока жива Database.
class WASAFE_API Signal {
public:
    Signal() = default;

    [[nodiscard]] bool valid() const noexcept { return db_ != nullptr && node_.valid(); }

    // --- идентификация ------------------------------------------------------
    [[nodiscard]] std::string_view name() const;  ///< локальное имя ("data", "[3]", ".addr")
    [[nodiscard]] std::string fullPath() const;   ///< "top.u_cpu.regfile[3].valid"
    [[nodiscard]] NodeId node() const noexcept { return node_; }

    // --- тип ----------------------------------------------------------------
    [[nodiscard]] Type type() const;
    [[nodiscard]] TypeKind kind() const;        ///< сокращение для type()->kind()
    [[nodiscard]] std::uint32_t width() const;  ///< битовая ширина для logic; иначе 0

    // --- структура ----------------------------------------------------------
    [[nodiscard]] bool isLeaf() const;                       ///< нет дочерних элементов
    [[nodiscard]] bool isComposite() const;                  ///< struct/union/array
    [[nodiscard]] bool hasOwnStream() const;                 ///< хранит собственный поток значений
    [[nodiscard]] std::optional<SignalId> streamId() const;  ///< id потока (для листьев)

    // --- единообразный обход вложенности -----------------------------------
    [[nodiscard]] std::size_t childCount() const;
    [[nodiscard]] Signal child(std::size_t ordinal) const;      ///< i-й член/элемент
    [[nodiscard]] Signal child(std::string_view member) const;  ///< член структуры по имени
    [[nodiscard]] Signal operator[](std::size_t index) const { return child(index); }
    /// Поиск ОТ ЭТОГО сигнала по относительному пути ("hdr.addr[2]"): члены и
    /// элементы, грамматика та же, что у Database::find. Пустой путь — сам
    /// сигнал. Невалидный Signal, если такого пути нет.
    [[nodiscard]] Signal find(std::string_view relative) const;
    [[nodiscard]] Signal parent() const;              ///< родительский узел (или невалидный)
    [[nodiscard]] SignalChildRange children() const;  ///< для range-based for

    // --- доступ к значениям (единый для листа и композита) ------------------
    /// Значение в момент времени t (последнее изменение на или до t).
    /// Для композита возвращает Aggregate, собранный из дочерних значений.
    [[nodiscard]] Value valueAt(TimeStamp t) const;

    /// Ленивый курсор по изменениям в диапазоне. Для композита итерирует
    /// объединённые моменты изменений дочерних элементов.
    [[nodiscard]] ValueCursor changes(TimeRange range) const;

    /// Навигация по «фронтам» (только для листьев; для композита — ближайший
    /// фронт среди дочерних). kNoTime — изменений нет.
    [[nodiscard]] TimeStamp nextChange(TimeStamp after) const;
    [[nodiscard]] TimeStamp prevChange(TimeStamp before) const;

    [[nodiscard]] const Database& database() const noexcept { return *db_; }

    friend bool operator==(const Signal&, const Signal&) = default;
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

private:
    friend class Database;
    friend class Scope;
    friend class SignalChildRange;

private:
    Signal(const Database* db, NodeId node) noexcept : db_{db}, node_{node} {}

private:
    const Database* db_ = nullptr;
    NodeId node_;
};

/// Ленивый диапазон сигналов для range-based for. Итерирует ЯВНЫЙ массив
/// индексов узлов — поэтому одинаково обслуживает и дочерние элементы композита
/// (Hierarchy::SignalNode::children) и сигналы верхнего уровня scope
/// (Hierarchy::ScopeNode::signals).
class WASAFE_API SignalChildRange {
public:
    class Iterator {
    public:
        using IteratorCategory = std::forward_iterator_tag;
        using ValueType = Signal;
        using DifferenceType = std::ptrdiff_t;
        using Reference = Signal;
        using Pointer = void;

    public:
        Iterator() = default;
        Iterator(const Database* db, const NodeId* nodes, std::size_t pos) noexcept :
                db_{db}, nodes_{nodes}, pos_{pos} {}

        [[nodiscard]] Signal operator*() const;
        Iterator& operator++() noexcept {
            ++pos_;
            return *this;
        }
        Iterator operator++(int) noexcept {
            auto t = *this;
            ++pos_;
            return t;
        }
        [[nodiscard]] bool operator==(const Iterator& o) const noexcept { return pos_ == o.pos_; }

    private:
        const Database* db_ = nullptr;
        const NodeId* nodes_ = nullptr;
        std::size_t pos_ = 0;
    };

public:
    /// @param nodes указатель на непрерывный массив индексов узлов (живёт в Hierarchy)
    SignalChildRange(const Database* db, const NodeId* nodes, std::size_t count) noexcept :
            db_{db}, nodes_{nodes}, count_{count} {}

    [[nodiscard]] Iterator begin() const noexcept { return {db_, nodes_, 0}; }
    [[nodiscard]] Iterator end() const noexcept { return {db_, nodes_, count_}; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    const Database* db_;
    const NodeId* nodes_;
    std::size_t count_;
};

}  // namespace WaSafe
