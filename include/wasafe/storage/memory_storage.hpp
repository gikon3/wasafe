#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

#include "wasafe/export.hpp"
#include "wasafe/storage/storage.hpp"
#include "wasafe/types/column_view.hpp"
#include "wasafe/types/time_column.hpp"

namespace WaSafe {

/// Backend, хранящий все изменения значений в ОЗУ. Подходит для небольших
/// дампов и как приёмник ingestion (MemoryBuilder наполняет именно его).
///
/// Внутреннее представление — поколоночное: для каждого потока массив меток
/// времени и плотно упакованные значения (битовые планы для logic, double для
/// real, арена для строк). Это даёт быстрый бинарный поиск по времени.
class WASAFE_API MemoryStorage final : public Storage {
public:
    // Storage
    [[nodiscard]] TimeRange timeRange() const override { return timeRange_; }
    [[nodiscard]] TimeScale timeScale() const override { return timeScale_; }
    [[nodiscard]] ValueView valueAt(SignalId id, TimeStamp timestamp) const override;
    [[nodiscard]] std::unique_ptr<Cursor> openCursor(SignalId id, TimeRange range) const override;
    [[nodiscard]] TimeStamp nextChange(SignalId id, TimeStamp after) const override;
    [[nodiscard]] TimeStamp prevChange(SignalId id, TimeStamp before) const override;

    // Пакетная перегрузка openCursor(span) живёт в базовом классе: без using
    // объявление override одиночной скрыло бы её при поиске имени.
    using Storage::openCursor;

    // --- наполнение (вызывается из MemoryBuilder) ---------------------------
    void setTimeScale(TimeScale s) { timeScale_ = s; }
    void reserveStreams(std::size_t n);
    /// Зарегистрировать поток заданной ширины (0 — real/string).
    SignalId createStream(ValueKind kind, std::uint32_t width);
    /// Дописать изменение (время должно монотонно возрастать в пределах потока).
    void append(SignalId id, TimeStamp t, ValueView v);
    /// Завершить наполнение: уплотнить, вычислить общий диапазон времени.
    void finalize();

private:
    /// Плотное поколоночное хранилище одного потока. Внутреннее представление
    /// инкапсулировано: наполнение — только через конструктор и append(), чтение —
    /// через аксессоры и невладеющий columns(). Вид потока задаётся активной
    /// альтернативой values_ — отдельного поля kind не требуется.
    class Stream final {
    public:
        Stream(ValueKind kind, std::uint32_t width);

        void append(TimeStamp t, ValueView v);
        [[nodiscard]] bool empty() const noexcept { return times_.empty(); }
        [[nodiscard]] const TimeColumn& times() const noexcept { return times_; }
        [[nodiscard]] ColumnView columns() const noexcept;

    private:
        /// Планы хранятся отдельно; bval пуст, пока в потоке не встретилось x/z
        struct LogicStore {
            std::uint32_t width = 0;
            std::vector<std::uint64_t> aval;
            std::vector<std::uint64_t> bval;
        };
        struct RealStore {
            std::vector<double> values;
        };
        struct StringStore {
            std::vector<char> arena;
            std::vector<std::uint32_t> offsets;
        };

    private:
        TimeColumn times_;
        std::variant<std::monostate, LogicStore, RealStore, StringStore> values_;
    };

private:
    [[nodiscard]] const Stream* stream(SignalId id) const noexcept;

private:
    std::vector<Stream> streams_;
    TimeRange timeRange_{};
    TimeScale timeScale_{};
};

}  // namespace WaSafe
