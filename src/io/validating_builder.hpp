#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wasafe/io/builder.hpp"

namespace WaSafe {

/// Декоратор поверх любого Builder'а, проверяющий контракт приёмника.
///
/// Контракт Builder рассчитан на ВНЕШНИЕ парсеры, но сам приёмник ради горячего
/// пути почти ничего не проверяет, и нарушения выходят наружу не тем, чем
/// обещано моделью ошибок:
///   - несовпадение вида значения с объявленным потоком доходит до
///     std::bad_variant_access из ValueView::logic()/real(), а не Exception;
///   - слишком ДЛИННОЕ значение молча обрезается по ширине потока;
///   - слишком КОРОТКОЕ пишет за границу массива. Биты за width() читаются как
///     X (LogicVectorView::operator[]), у X b-бит единичный, а план bval у
///     двухзначного блока не заведён вовсе — узкое значение двухзначно, и
///     MemoryStorage::Stream::append (как и DecodedBlock::append) его не
///     разворачивает. Запись уходит в пустой вектор.
///
/// Декоратор ставится на время отладки формата и превращает это в
/// WaSafe::Exception с внятным текстом. В готовом парсере он не нужен: каждый
/// valueChange стоит поиска в хеш-таблице.
class ValidatingBuilder final : public Builder {
public:
    explicit ValidatingBuilder(Builder& sink) noexcept : sink_{sink} {}

    void setTimeScale(TimeScale scale) override;
    ScopeId beginScope(std::string_view name, ScopeKind kind) override;
    void endScope() override;
    SignalId declareVar(std::string_view name, Type type, std::optional<SignalId> alias,
            std::span<SignalId> leaves) override;
    void headerDone() override;
    void setTime(TimeStamp time) override;
    void valueChange(SignalId id, ValueView value) override;
    void finish() override;
    [[nodiscard]] Database takeDatabase() override;

private:
    /// Вид и ширина потока — ровно то, что BaseBuilder::makeStream() передал в
    /// allocStream(). Больше о потоке знать нечего: storage хранит именно это.
    struct Stream {
        ValueKind kind = ValueKind::NONE;
        std::uint32_t width = 0;
    };

private:
    /// Типы элементов в порядке разворачивания — тот же обход, что и в
    /// BaseBuilder::buildChildren(): в глубину, элементы массива по ordinalOf,
    /// члены структуры в порядке объявления, packed-поддерево не расходует
    /// потоков. Размер результата равен expansionStreamCount(type).
    static std::vector<Type> expansionTypes(const Type& type);

    void requireHeader(std::string_view method) const;
    void requireValues(std::string_view method) const;
    void requireKnown(SignalId id, std::string_view method, std::string_view what) const;
    void record(SignalId id, const Type& t);

private:
    // Декоратор привязан к приёмнику вызывающего на всю свою жизнь: в контейнер
    // не кладётся и не переприсваивается, поэтому терять value-семантику нечего.
    // Ссылка, а не указатель: нулевого и перевязываемого состояния у него быть
    // не должно.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
    Builder& sink_;
    std::unordered_map<SignalId, Stream> streams_;
    std::size_t openScopes_ = 0;
    TimeStamp now_ = 0;
    bool headerDone_ = false;
    bool timeSet_ = false;
    bool finished_ = false;
    bool taken_ = false;
};

}  // namespace WaSafe
