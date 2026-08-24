#pragma once

#include <filesystem>
#include <string_view>

#include "wasafe/core/exception.hpp"
#include "wasafe/export.hpp"

namespace WaSafe {

class Database;

/// Абстрактный сериализатор: экспорт хранилища обратно в формат (VCD/FST/...).
/// Симметричен Reader: читает БД через её публичный интерфейс и пишет файл.
class WASAFE_API Writer {
public:
    virtual ~Writer() = default;

    [[nodiscard]] virtual std::string_view format() const = 0;

    /// Записать всю БД в файл.
    virtual void write(const Database& db, const std::filesystem::path& path) = 0;

protected:
    Writer() = default;
    Writer(const Writer&) = default;
    Writer(Writer&&) = default;

    Writer& operator=(const Writer&) = default;
    Writer& operator=(Writer&&) = default;
};

}  // namespace WaSafe
