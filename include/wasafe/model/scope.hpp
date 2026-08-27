#pragma once

#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

#include "wasafe/core/ids.hpp"
#include "wasafe/export.hpp"
#include "wasafe/model/signal.hpp"

namespace WaSafe {

class Database;

/// Вид узла иерархии.
enum class ScopeKind : std::uint8_t {
    ROOT,
    MODULE,
    INTERFACE,
    PACKAGE,
    PROGRAM,
    TASK,
    FUNCTION,
    BLOCK,  ///< begin/end, fork/join
    GENERATE_BLOCK,
    STRUCT,  ///< экземпляр структуры как уровень иерархии
    UNION,
    ARRAY,  ///< развёрнутый массив инстансов/элементов
    CLASS,
    UNKNOWN,
};

[[nodiscard]] WASAFE_API std::string_view toString(ScopeKind k) noexcept;

class ScopeRange;

/// Невладеющий дескриптор узла иерархии (scope). Лёгкий хэндл.
class WASAFE_API Scope {
public:
    Scope() = default;

    [[nodiscard]] bool valid() const noexcept { return db_ != nullptr && id_.valid(); }

    [[nodiscard]] std::string_view name() const;
    [[nodiscard]] std::string fullPath() const;
    [[nodiscard]] ScopeKind kind() const;
    [[nodiscard]] ScopeId id() const noexcept { return id_; }
    [[nodiscard]] Scope parent() const;

    // Вложенные scope.
    [[nodiscard]] std::size_t scopeCount() const;
    [[nodiscard]] Scope scope(std::size_t index) const;
    [[nodiscard]] Scope scope(std::string_view name) const;
    [[nodiscard]] ScopeRange scopes() const;

    // Сигналы, объявленные непосредственно в этом scope.
    [[nodiscard]] std::size_t signalCount() const;
    [[nodiscard]] Signal signal(std::size_t index) const;
    [[nodiscard]] Signal signal(std::string_view name) const;
    [[nodiscard]] SignalChildRange signals() const;

    // Поиск по относительному пути (грамматика та же, что у Database::find).
    /// Сигнал по пути от этого scope ("alu.result[3]"): вложенные scope, затем
    /// сигнал. Пустой путь именует scope, а не сигнал, поэтому даёт невалидный
    /// Signal — как и любой ненайденный путь.
    [[nodiscard]] Signal find(std::string_view relative) const;
    /// Вложенный scope по пути от этого scope; пустой путь — сам scope.
    [[nodiscard]] Scope findScope(std::string_view relative) const;

    friend bool operator==(const Scope&, const Scope&) = default;
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

private:
    friend class Database;

private:
    Scope(const Database* db, ScopeId id) noexcept : db_{db}, id_{id} {}

    /// Проверка хэндла на входе публичных методов.
    void require() const {
        if (!valid())
            throwInvalid();
    }

    [[noreturn]] static void throwInvalid();

private:
    const Database* db_ = nullptr;
    ScopeId id_;
};

/// Ленивый диапазон вложенных scope для range-based for.
class WASAFE_API ScopeRange {
public:
    class Iterator {
    public:
        using IteratorCategory = std::forward_iterator_tag;
        using ValueType = Scope;
        using DifferenceType = std::ptrdiff_t;
        using Reference = Scope;
        using Pointer = void;

    public:
        Iterator() = default;
        Iterator(const Database* db, ScopeId parent, std::size_t pos) noexcept : db_{db}, parent_{parent}, pos_{pos} {}

        [[nodiscard]] Scope operator*() const;
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
        ScopeId parent_;
        std::size_t pos_ = 0;
    };

public:
    ScopeRange(const Database* db, ScopeId parent, std::size_t count) noexcept :
            db_{db}, parent_{parent}, count_{count} {}

    [[nodiscard]] Iterator begin() const noexcept { return {db_, parent_, 0}; }
    [[nodiscard]] Iterator end() const noexcept { return {db_, parent_, count_}; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    const Database* db_;
    ScopeId parent_;
    std::size_t count_;
};

}  // namespace WaSafe
