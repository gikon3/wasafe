#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wasafe/core/ids.hpp"
#include "wasafe/core/string_map.hpp"
#include "wasafe/export.hpp"
#include "wasafe/model/scope.hpp"
#include "wasafe/types/type.hpp"

namespace WaSafe {

/// Битовый срез родительского logic-потока. Нужен для packed-структур/массивов,
/// которые в дампе хранятся ОДНИМ вектором: член/элемент — это срез
/// [offset, offset+width) потока предка. Отсутствие среза (свой поток или сборка
/// из детей) выражается пустым std::optional<BitSlice>.
struct BitSlice {
    std::uint32_t offset = 0;  ///< младший бит среза внутри родителя
    std::uint32_t width = 0;   ///< ширина среза
};

/// Иерархия дизайна: владеет всеми узлами scope и сигналов и предоставляет
/// поиск по пути. Значения здесь не хранятся (см. Storage). Строится
/// через Builder; пользователю доступна только для чтения.
class WASAFE_API Hierarchy final {
public:
    /// Узел иерархии-контейнера (модуль, интерфейс, генерируемый блок и т.п.).
    struct ScopeNode {
        std::string name;
        ScopeKind kind = ScopeKind::UNKNOWN;
        ScopeId parent;  ///< невалидный у корня
        std::vector<ScopeId> childScopes;
        std::vector<NodeId> signals;    ///< сигналы верхнего уровня в scope
        StringMap<ScopeId> scopeIndex;  ///< имя -> дочерний scope
        StringMap<NodeId> signalIndex;  ///< имя -> сигнал
    };

    /// Узел-сигнал. Может быть листом (own_stream) и/или композитом (children).
    /// Packed-член ссылается на поток предка через projection.
    struct SignalNode {
        std::string name;  ///< локальное имя ("data", "[3]", "addr")
        Type type;
        ScopeId scope;                       ///< владеющий scope (для узлов верхнего уровня)
        NodeId parent;                       ///< родительский SignalNode для членов/элементов (invalid у корня)
        SignalId stream;                     ///< собственный поток значений (invalid, если нет)
        std::optional<BitSlice> projection;  ///< срез потока предка, если своего потока нет
        std::vector<NodeId> children;        ///< члены структуры / элементы массива
        StringMap<NodeId> memberIndex;       ///< имя члена -> узел
    };

    /// Разбивка памяти, занятой метаданными иерархии.
    ///
    /// Счёт аналитический: он видит capacity контейнеров, но не округления
    /// аллокатора и не фрагментацию, поэтому это НИЖНЯЯ граница — RSS процесса
    /// будет заметно больше.
    struct MemoryUse {
        std::size_t nodes = 0;     ///< вектор узлов-сигналов: capacity x sizeof
        std::size_t scopes = 0;    ///< вектор узлов-scope
        std::size_t names = 0;     ///< heap имён -- ОБЕ копии: в узле и ключом карты
        std::size_t indexes = 0;   ///< StringMap: bucket-массивы и узлы, без heap ключей
        std::size_t children = 0;  ///< векторы childScopes / signals / children

        [[nodiscard]] std::size_t total() const noexcept;
    };

public:
    Hierarchy();

    // --- построение (используется реализациями Builder) -------------
    [[nodiscard]] ScopeId root() const noexcept { return kRootId; }
    ScopeId addScope(ScopeId parent, std::string name, ScopeKind kind);
    /// Объявить сигнал верхнего уровня в scope. Для композитного type вызывающая
    /// сторона затем добавляет детей через add_member/add_element.
    NodeId addSignal(ScopeId scope, std::string name, Type type, SignalId stream);
    /// Добавить член структуры/объединения к узлу-композиту.
    NodeId addMember(NodeId parent, std::string name, Type type, SignalId stream,
            std::optional<BitSlice> projection = std::nullopt);
    /// Добавить элемент массива (имя формируется как "[index]").
    NodeId addElement(NodeId parent, std::int32_t index, Type type, SignalId stream,
            std::optional<BitSlice> projection = std::nullopt);

    // --- доступ (используется Scope/Signal) ---------------------------------
    [[nodiscard]] const ScopeNode& scopeNode(ScopeId id) const {
        requireScope(id);
        return scopes_[id.get()];
    }
    [[nodiscard]] const SignalNode& signalNode(NodeId i) const {
        requireNode(i);
        return nodes_[i.get()];
    }
    [[nodiscard]] std::size_t scopeCount() const noexcept { return scopes_.size(); }
    [[nodiscard]] std::size_t signalCount() const noexcept { return nodes_.size(); }

    /// Во что обходятся метаданные этой иерархии. Проход по всем узлам и картам.
    [[nodiscard]] MemoryUse memoryUse() const;

    /// Поиск сигнала по полному иерархическому пути ("top.u1.data[3].field").
    /// Разбирает '.', индексацию '[i]' и доступ к членам единообразно.
    /// Частный случай поиска от корня.
    [[nodiscard]] std::optional<NodeId> findSignal(std::string_view path) const;
    /// Поиск scope по пути. Частный случай поиска от корня.
    [[nodiscard]] std::optional<ScopeId> findScope(std::string_view path) const;

    /// Поиск ОТ УЗЛА-СИГНАЛА: путь из членов и элементов ("hdr.addr[2]").
    /// Спуска по scope нет — внутри сигнала их не бывает. Пустой путь именует
    /// сам узел: он уже является сигналом.
    [[nodiscard]] std::optional<NodeId> findSignal(NodeId from, std::string_view relative) const;
    /// Поиск ОТ SCOPE: вложенные scope, затем сигнал ("alu.result[3]").
    /// Пустой путь даёт nullopt — он именует scope, а не сигнал.
    [[nodiscard]] std::optional<NodeId> findSignal(ScopeId from, std::string_view relative) const;
    /// Поиск scope ОТ SCOPE. Пустой путь именует сам scope.
    [[nodiscard]] std::optional<ScopeId> findScope(ScopeId from, std::string_view relative) const;

    /// Полный путь до узла.
    [[nodiscard]] std::string pathOf(NodeId node) const;
    [[nodiscard]] std::string pathOf(ScopeId scope) const;

private:
    /// Спуск от узла по разобранным на '.' сегментам пути: имя члена и/или
    /// ключи элементов ("[3]") в каждом сегменте. Общая часть всех перегрузок
    /// поиска — грамматика пути живёт в одном месте.
    [[nodiscard]] std::optional<NodeId> descend(NodeId node, std::span<const std::string_view> segments) const;

private:
    [[noreturn]] static void throwInvalidScope(ScopeId id, std::size_t count);
    [[noreturn]] static void throwInvalidNode(NodeId node, std::size_t count);
    /// Проверка хэндла на входе публичных методов. Условие `>= size` ловит и
    /// невалидный сентинел (0xFFFF'FFFF), и любой индекс за пределами. Быстрый
    /// путь инлайнится, бросок вынесен в холодную [[noreturn]]-функцию.
    void requireScope(ScopeId id) const {
        if (id.get() >= scopes_.size())
            throwInvalidScope(id, scopes_.size());
    }
    void requireNode(NodeId i) const {
        if (i.get() >= nodes_.size())
            throwInvalidNode(i, nodes_.size());
    }

private:
    static constexpr ScopeId kRootId{0};
    std::vector<ScopeNode> scopes_;
    std::vector<SignalNode> nodes_;
};

}  // namespace WaSafe
