#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace WaSafe {

/// Запись бинарного потока в ЯВНОМ little-endian — независимо от порядка байт
/// машины. Это обязательное свойство: файл store переживает процесс и может
/// быть перенесён на другую машину.
///
/// Скаляры собираются побайтово (сдвигами), поэтому код одинаков на любой
/// архитектуре. Массивы на LE-машине уходят одним memcpy, на BE — поэлементно;
/// ветвление разрешается на этапе компиляции, так что на x86/ARM цена нулевая.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::byte>& out) noexcept : out_{out} {}

    void u8(std::uint8_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i32(std::int32_t v);
    void i64(std::int64_t v);
    void f64(double v);

    /// LEB128: беззнаковое число переменной длины.
    void varint(std::uint64_t v);
    /// Зигзаг + LEB128: знаковое, малое по модулю укладывается в один-два байта.
    void svarint(std::int64_t v);

    /// Длина (varint) и сами байты.
    void str(std::string_view v);
    /// Сырые байты без длины — длину пишет вызывающий, если она нужна.
    void bytes(std::span<const std::byte> v);

    void array(std::span<const std::uint32_t> v);
    void array(std::span<const std::uint64_t> v);
    void array(std::span<const double> v);

    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }

private:
    std::vector<std::byte>& out_;
};

/// Чтение потока, записанного ByteWriter. Все методы возвращают false при
/// выходе за границу буфера и не бросают: разбор повреждённого файла — это
/// ожидаемая ситуация, а не исключение.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : data_{data} {}

    [[nodiscard]] bool u8(std::uint8_t& out) noexcept;
    [[nodiscard]] bool u32(std::uint32_t& out) noexcept;
    [[nodiscard]] bool u64(std::uint64_t& out) noexcept;
    [[nodiscard]] bool i32(std::int32_t& out) noexcept;
    [[nodiscard]] bool i64(std::int64_t& out) noexcept;
    [[nodiscard]] bool f64(double& out) noexcept;

    [[nodiscard]] bool varint(std::uint64_t& out) noexcept;
    [[nodiscard]] bool svarint(std::int64_t& out) noexcept;

    [[nodiscard]] bool str(std::string& out);
    [[nodiscard]] bool bytes(std::span<std::byte> out) noexcept;

    /// Массивы читаются в уже размеченный по размеру приёмник.
    [[nodiscard]] bool array(std::span<std::uint32_t> out) noexcept;
    [[nodiscard]] bool array(std::span<std::uint64_t> out) noexcept;
    [[nodiscard]] bool array(std::span<double> out) noexcept;

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

}  // namespace WaSafe
