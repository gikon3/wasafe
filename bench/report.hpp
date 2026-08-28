#pragma once

#include <chrono>
#include <format>
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
    using Cells = std::map<std::string, std::string>;
    using Rows = std::vector<std::pair<std::string, Cells>>;

public:
    class Row {
    public:
        explicit Row(Cells& cells) : cells_{cells} {}

        Row& num(const std::string& key, double value, int precision = 1) {
            cells_[key] = std::format("{:.{}f}", value, precision);
            return *this;
        }

        Row& text(const std::string& key, std::string value) {
            cells_[key] = std::move(value);
            return *this;
        }

    private:
        // Row — короткоживущий прокси, возвращаемый Report::add() и тут же
        // используемый цепочкой .num().text(). В контейнер не кладётся и не
        // переприсваивается, поэтому терять value-семантику нечего.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
        Cells& cells_;
    };

public:
    Row add(std::string name) {
        rows_.emplace_back(std::move(name), Cells{});
        return Row{rows_.back().second};
    }

    /// csv — машиночитаемо, иначе выровненная таблица для чтения глазами.
    void print(bool csv) const;

private:
    Rows rows_;
};

}  // namespace Bench
