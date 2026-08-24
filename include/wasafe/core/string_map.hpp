#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace WaSafe {

/// Прозрачный хеш для строк: позволяет искать в map по std::string_view без
/// создания временного std::string (гетерогенный поиск, C++20+).
struct TransparentStringHash {
    // NOLINTNEXTLINE(readability-identifier-naming)
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    [[nodiscard]] std::size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    [[nodiscard]] std::size_t operator()(const char* s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view{s});
    }
};

/// unordered_map с ВЛАДЕЮЩИМ строковым ключом и гетерогенным поиском.
/// Владение ключом важно: узлы хранятся в растущих vector, и string_view-ключи
/// «повисли» бы при реаллокации.
template <class V>
using StringMap = std::unordered_map<std::string, V, TransparentStringHash, std::equal_to<>>;

}  // namespace WaSafe
