#pragma once

#include <compare>
#include <cstdint>
#include <functional>

namespace WaSafe {

template <class Tag>
class StrongId final {
public:
    using ValueType = std::uint32_t;

public:
    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(ValueType v) noexcept : value_{v} {}

    [[nodiscard]] constexpr ValueType get() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != kInvalid; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

    friend auto operator<=>(StrongId, StrongId) = default;

private:
    static constexpr ValueType kInvalid = 0xFFFF'FFFFu;
    ValueType value_ = kInvalid;
};

struct SignalIdTag {};
struct ScopeIdTag {};
struct NodeIdTag {};

/// Идентификатор листового потока изменений значений внутри storage.
/// Несколько Signal могут ссылаться на один SignalId (алиасы VCD).
using SignalId = StrongId<SignalIdTag>;

/// Идентификатор узла иерархии (scope: модуль, интерфейс, struct-экземпляр ...).
using ScopeId = StrongId<ScopeIdTag>;

/// Индекс узла-сигнала внутри Hierarchy (включая нелистовые композиты).
/// Узел может не иметь собственного SignalId (например, unpacked-структура).
using NodeId = StrongId<NodeIdTag>;

}  // namespace WaSafe

// Хеши для unordered-контейнеров. Специализация std::hash для пользовательского
// типа явно разрешена стандартом ([namespace.std]). Открытый namespace, а не
// квалифицированное `struct ::std::hash<...>`: ведущее `::` в определении класса
// GCC не принимает.
namespace std {

template <class Tag>
struct hash<WaSafe::StrongId<Tag>> {  // NOLINT(bugprone-std-namespace-modification)
    std::size_t operator()(WaSafe::StrongId<Tag> id) const noexcept { return std::hash<std::uint32_t>{}(id.get()); }
};

}  // namespace std
