#pragma once

#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Bench {

/// Секундомер по монотонным часам. Замеры здесь секундного масштаба (разбор
/// дампа, проход курсором), поэтому дрожание планировщика значения не имеет и
/// усреднять по тысячам итераций не нужно.
class Timer {
public:
    Timer() : start_{std::chrono::steady_clock::now()} {}

    [[nodiscard]] double ms() const {
        const auto d = std::chrono::steady_clock::now() - start_;
        return std::chrono::duration<double, std::milli>(d).count();
    }

private:
    std::chrono::steady_clock::time_point start_;
};

/// Таблица результатов с произвольным набором колонок: у разных случаев меряются
/// разные величины, а сводить их в одну жёсткую схему было бы враньём.
class Report {
public:
    class Row {
    public:
        explicit Row(std::map<std::string, std::string>& cells) : cells_{cells} {}

        Row& num(const std::string& key, double value, int precision = 1) {
            std::string buf(32, '\0');
            const int n = std::snprintf(buf.data(), buf.size(), "%.*f", precision, value);
            buf.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
            cells_[key] = buf;
            return *this;
        }

        Row& text(const std::string& key, std::string value) {
            cells_[key] = std::move(value);
            return *this;
        }

    private:
        std::map<std::string, std::string>& cells_;
    };

public:
    Row add(std::string name) {
        rows_.emplace_back(std::move(name), std::map<std::string, std::string>{});
        return Row{rows_.back().second};
    }

    /// csv — машиночитаемо, иначе выровненная таблица для чтения глазами.
    void print(bool csv) const;

private:
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> rows_;
};

}  // namespace Bench
