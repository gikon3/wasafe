#pragma once

#include <cstdint>
#include <memory>

#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"
#include "wasafe/types/value.hpp"

namespace WaSafe {

/// Одно изменение значения: момент времени + значение (невладеющее) + указание
/// на источник, если курсор слил несколько потоков.
struct ValueChange {
    TimeStamp time = kNoTime;
    ValueView value;
    /// Индекс источника в span, переданном в openCursor()/changes(). У курсора
    /// по одному источнику всегда 0. Индекс, а не SignalId: алиасы VCD дают
    /// несколько Signal на один поток, а два packed-члена одного вектора — одну
    /// и ту же проекцию, поэтому SignalId не различает запрошенное.
    std::uint32_t source = 0;
};

/// Интерфейс «тянущего» курсора по изменениям в заданном диапазоне. Источник —
/// один поток либо несколько, слитых в общий хронологический порядок (тогда
/// current().source говорит, чьё изменение отдано). Реализуется storage'ом.
/// Курсор владеет своими буферами, поэтому ValueView, полученный из current(),
/// валиден до следующего вызова next().
class WASAFE_API Cursor {
public:
    virtual ~Cursor() = default;

    /// Перейти к следующему изменению. false — данные закончились.
    [[nodiscard]] virtual bool next() = 0;

    /// Текущее изменение (валидно после успешного next()).
    [[nodiscard]] virtual const ValueChange& current() const noexcept = 0;

protected:
    Cursor() = default;
    Cursor(const Cursor&) = default;
    Cursor(Cursor&&) = default;

    Cursor& operator=(const Cursor&) = default;
    Cursor& operator=(Cursor&&) = default;
};

/// Пользовательская обёртка над Cursor. Семантика однонаправленного итератора.
/// Пример:
///   auto cur = signal.changes({1000, 2000});
///   while (cur.next()) { use(cur->time, cur->value); }
class WASAFE_API ValueCursor {
public:
    ValueCursor() = default;
    explicit ValueCursor(std::unique_ptr<Cursor> impl) : impl_(std::move(impl)) {}
    ValueCursor(const ValueCursor&) = delete;
    ValueCursor(ValueCursor&&) = default;
    virtual ~ValueCursor() = default;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(impl_); }

    /// Продвинуть курсор. false — конец диапазона / пустой курсор.
    [[nodiscard]] bool next() { return impl_ && impl_->next(); }

    [[nodiscard]] const ValueChange& current() const noexcept { return impl_->current(); }
    [[nodiscard]] const ValueChange* operator->() const noexcept { return &impl_->current(); }
    [[nodiscard]] const ValueChange& operator*() const noexcept { return impl_->current(); }

    ValueCursor& operator=(const ValueCursor&) = delete;
    ValueCursor& operator=(ValueCursor&&) = default;

private:
    std::unique_ptr<Cursor> impl_;
};

}  // namespace WaSafe
