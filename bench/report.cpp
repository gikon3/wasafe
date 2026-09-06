#include "report.hpp"

#include <algorithm>
#include <print>
#include <string_view>

#ifdef __linux__
#include <unistd.h>

#include <fstream>
#include <tuple>
#endif
#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace Bench {

std::size_t residentBytes() {
#ifdef __linux__
    // statm, а не status: два первых числа разбираются потоком без поиска ключа
    // по строкам, а функция зовётся вокруг каждого замера. Первое поле — размер
    // образа, второе — резидентные СТРАНИЦЫ, отсюда умножение на размер страницы.
    std::ifstream f{"/proc/self/statm"};
    unsigned long long total = 0;
    unsigned long long resident = 0;
    if (!(f >> total >> resident))
        return 0;
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0)
        return 0;
    return static_cast<std::size_t>(resident) * static_cast<std::size_t>(page);
#else
    return 0;
#endif
}

void releaseFreedMemory() {
#ifdef __GLIBC__
    std::ignore = malloc_trim(0);
#endif
}

namespace {

/// Колонки собираются по всем строкам: у случаев они разные.
std::vector<std::string> collectColumns(const Report::Rows& rows) {
    std::vector<std::string> columns;
    for (const auto& [name, cells] : rows) {
        for (const auto& [key, value] : cells) {
            if (std::ranges::find(columns, key) == columns.end())
                columns.push_back(key);
        }
    }
    return columns;
}

void printCsv(const Report::Rows& rows, const std::vector<std::string>& columns, std::string_view first) {
    std::print("{}", first);
    for (const auto& c : columns)
        std::print(",{}", c);
    std::print("\n");

    for (const auto& [name, cells] : rows) {
        std::print("{}", name);
        for (const auto& c : columns) {
            const auto it = cells.find(c);
            std::print(",{}", it == cells.end() ? "" : it->second);
        }
        std::print("\n");
    }
}

/// Ширина каждой колонки — максимум по заголовку и всем значениям.
std::vector<std::size_t> columnWidths(const Report::Rows& rows, const std::vector<std::string>& columns,
        std::string_view first) {
    std::vector<std::size_t> width;
    width.push_back(first.size());
    for (const auto& [name, cells] : rows)
        width[0] = std::max(width[0], name.size());

    for (const auto& c : columns) {
        std::size_t w = c.size();
        for (const auto& [name, cells] : rows) {
            const auto it = cells.find(c);
            if (it != cells.end())
                w = std::max(w, it->second.size());
        }
        width.push_back(w);
    }
    return width;
}

void printAligned(const Report::Rows& rows, const std::vector<std::string>& columns, std::string_view first) {
    const std::vector<std::size_t> width = columnWidths(rows, columns, first);

    std::print("{:<{}}", first, width[0]);
    for (std::size_t i = 0; i < columns.size(); ++i)
        std::print("  {:>{}}", columns[i], width[i + 1]);
    std::print("\n");

    for (const auto& [name, cells] : rows) {
        std::print("{:<{}}", name, width[0]);
        for (std::size_t i = 0; i < columns.size(); ++i) {
            const auto it = cells.find(columns[i]);
            std::print("  {:>{}}", it == cells.end() ? "-" : it->second, width[i + 1]);
        }
        std::print("\n");
    }
}

}  // namespace

void Report::print(bool csv) const {
    if (rows_.empty())
        return;

    constexpr std::string_view kFirst = "case";
    const std::vector<std::string> columns = collectColumns(rows_);

    if (csv)
        printCsv(rows_, columns, kFirst);
    else
        printAligned(rows_, columns, kFirst);
}

}  // namespace Bench
