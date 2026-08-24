#pragma once

#include <string_view>

#include "wasafe/core/exception.hpp"
#include "wasafe/export.hpp"
#include "wasafe/io/builder.hpp"

namespace WaSafe {

/// Абстрактный парсер формата. Реализация читает источник и наполняет хранилище
/// через WaveformBuilder — единый приёмник для всех форматов.
class WASAFE_API Reader {
public:
    virtual ~Reader() = default;

    [[nodiscard]] virtual std::string_view format() const = 0;

    /// Полный разбор источника в builder (фазы заголовка и значений).
    virtual void read(Builder& sink) = 0;

    /// Опционально: разобрать только заголовок (иерархия+типы) без значений —
    /// нужно ленивому открытию. По умолчанию делегирует к read().
    virtual void readHeader(Builder& sink) { read(sink); }

protected:
    Reader() = default;
    Reader(const Reader&) = default;
    Reader(Reader&&) = default;

    Reader& operator=(const Reader&) = default;
    Reader& operator=(Reader&&) = default;
};

}  // namespace WaSafe
