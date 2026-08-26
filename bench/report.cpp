#include "report.hpp"

#include <algorithm>
#include <print>

namespace Bench {

void Report::print(bool csv) const {
    if (rows_.empty())
        return;

    // Колонки собираются по всем строкам: у случаев они разные.
    std::vector<std::string> columns;
    for (const auto& [name, cells] : rows_) {
        for (const auto& [key, value] : cells) {
            if (std::ranges::find(columns, key) == columns.end())
                columns.push_back(key);
        }
    }

    const std::string first = "case";
    if (csv) {
        std::print("{}", first);
        for (const auto& c : columns)
            std::print(",{}", c);
        std::print("\n");
        for (const auto& [name, cells] : rows_) {
            std::print("{}", name);
            for (const auto& c : columns) {
                const auto it = cells.find(c);
                std::print(",{}", it == cells.end() ? "" : it->second);
            }
            std::print("\n");
        }
        return;
    }

    std::vector<std::size_t> width;
    width.push_back(first.size());
    for (const auto& [name, cells] : rows_)
        width[0] = std::max(width[0], name.size());
    for (const auto& c : columns) {
        std::size_t w = c.size();
        for (const auto& [name, cells] : rows_) {
            const auto it = cells.find(c);
            if (it != cells.end())
                w = std::max(w, it->second.size());
        }
        width.push_back(w);
    }

    std::print("{:<{}}", first, width[0]);
    for (std::size_t i = 0; i < columns.size(); ++i)
        std::print("  {:>{}}", columns[i], width[i + 1]);
    std::print("\n");

    for (const auto& [name, cells] : rows_) {
        std::print("{:<{}}", name, width[0]);
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const auto it = cells.find(columns[i]);
            std::print("  {:>{}}", it == cells.end() ? "-" : it->second, width[i + 1]);
        }
        std::print("\n");
    }
}

}  // namespace Bench
