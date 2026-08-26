#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"
#include "wasafe/model/signal.hpp"

namespace WaSafe {

class Database;

/// Утилиты пакетного поиска и выборки сигналов.
///
/// Дополняют точечный Database::find() выборками по шаблону имени и
/// массовой подгрузкой диапазонов (с подсказкой prefetch ленивому storage).
class WASAFE_API SignalQuery {
public:
    explicit SignalQuery(const Database& db) noexcept : db_{&db} {}

    /// Все сигналы, чей полный путь соответствует glob-шаблону ("top.*.clk",
    /// "top.cpu.regs[*]"). Поиск рекурсивный по всей иерархии.
    [[nodiscard]] std::vector<Signal> match(std::string_view glob) const;

    /// Все листовые сигналы поддерева scope (включая раскрытие композитов).
    [[nodiscard]] std::vector<Signal> leavesUnder(const Scope& scope) const;

    /// Произвольный фильтр по сигналам.
    [[nodiscard]] std::vector<Signal> where(const std::function<bool(const Signal&)>& pred) const;

    /// Сообщить storage о намерении читать эти сигналы в диапазоне (prefetch).
    void prefetch(const std::vector<Signal>& signals, TimeRange range) const;

private:
    const Database* db_;
};

}  // namespace WaSafe
