#include "core/byte_io.hpp"

#include <bit>
#include <cstring>

namespace WaSafe {

namespace {

constexpr bool kNativeLe = std::endian::native == std::endian::little;

/// Разложить беззнаковое в little-endian байты. Сдвиги, а не memcpy: код не
/// зависит от порядка байт машины.
template <class T>
void storeLe(std::byte* dst, T v) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i)
        dst[i] = static_cast<std::byte>((v >> (i * 8u)) & 0xFFu);
}

template <class T>
[[nodiscard]] T loadLe(const std::byte* src) noexcept {
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        v |= static_cast<T>(static_cast<std::uint8_t>(src[i])) << (i * 8u);
    return v;
}

/// Общее тело записи массива: на LE — один memcpy, иначе поэлементно.
template <class T, class Raw>
void writeArray(std::vector<std::byte>& out, std::span<const T> v) {
    if (v.empty())
        return;
    const std::size_t off = out.size();
    out.resize(off + (v.size() * sizeof(T)));
    if constexpr (kNativeLe) {
        std::memcpy(out.data() + off, v.data(), v.size() * sizeof(T));
    }
    else {
        for (std::size_t i = 0; i < v.size(); ++i)
            storeLe<Raw>(out.data() + off + (i * sizeof(T)), std::bit_cast<Raw>(v[i]));
    }
}

template <class T, class Raw>
[[nodiscard]] bool readArray(std::span<const std::byte> data, std::size_t& pos, std::span<T> out) noexcept {
    if (out.empty())
        return true;
    const std::size_t bytes = out.size() * sizeof(T);
    if (pos + bytes > data.size())
        return false;
    if constexpr (kNativeLe) {
        std::memcpy(out.data(), data.data() + pos, bytes);
    }
    else {
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = std::bit_cast<T>(loadLe<Raw>(data.data() + pos + (i * sizeof(T))));
    }
    pos += bytes;
    return true;
}

/// Зигзаг: знак уезжает в младший бит, поэтому -1 стоит один байт, а не десять.
[[nodiscard]] std::uint64_t zigzag(std::int64_t v) noexcept {
    return (static_cast<std::uint64_t>(v) << 1u) ^ static_cast<std::uint64_t>(v >> 63);
}

[[nodiscard]] std::int64_t unzigzag(std::uint64_t v) noexcept {
    return static_cast<std::int64_t>((v >> 1u) ^ (~(v & 1u) + 1u));
}

template <class T>
void appendLe(std::vector<std::byte>& out, T v) {
    const std::size_t off = out.size();
    out.resize(off + sizeof(T));
    storeLe<T>(out.data() + off, v);
}

template <class T>
[[nodiscard]] bool takeLe(std::span<const std::byte> data, std::size_t& pos, T& out) noexcept {
    if (pos + sizeof(T) > data.size())
        return false;
    out = loadLe<T>(data.data() + pos);
    pos += sizeof(T);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// ByteWriter
// ---------------------------------------------------------------------------
void ByteWriter::u8(std::uint8_t v) {
    out_.push_back(static_cast<std::byte>(v));
}

void ByteWriter::u32(std::uint32_t v) {
    appendLe(out_, v);
}

void ByteWriter::u64(std::uint64_t v) {
    appendLe(out_, v);
}

void ByteWriter::i32(std::int32_t v) {
    appendLe(out_, static_cast<std::uint32_t>(v));
}

void ByteWriter::i64(std::int64_t v) {
    appendLe(out_, static_cast<std::uint64_t>(v));
}

void ByteWriter::f64(double v) {
    appendLe(out_, std::bit_cast<std::uint64_t>(v));
}

void ByteWriter::varint(std::uint64_t v) {
    while (v >= 0x80u) {
        out_.push_back(static_cast<std::byte>((v & 0x7Fu) | 0x80u));
        v >>= 7u;
    }
    out_.push_back(static_cast<std::byte>(v));
}

void ByteWriter::svarint(std::int64_t v) {
    varint(zigzag(v));
}

void ByteWriter::str(std::string_view v) {
    varint(v.size());
    const std::size_t off = out_.size();
    out_.resize(off + v.size());
    if (!v.empty())
        std::memcpy(out_.data() + off, v.data(), v.size());
}

void ByteWriter::bytes(std::span<const std::byte> v) {
    out_.insert(out_.end(), v.begin(), v.end());
}

void ByteWriter::array(std::span<const std::uint32_t> v) {
    writeArray<std::uint32_t, std::uint32_t>(out_, v);
}

void ByteWriter::array(std::span<const std::uint64_t> v) {
    writeArray<std::uint64_t, std::uint64_t>(out_, v);
}

void ByteWriter::array(std::span<const double> v) {
    writeArray<double, std::uint64_t>(out_, v);
}

// ---------------------------------------------------------------------------
// ByteReader
// ---------------------------------------------------------------------------
bool ByteReader::u8(std::uint8_t& out) noexcept {
    if (pos_ >= data_.size())
        return false;
    out = static_cast<std::uint8_t>(data_[pos_++]);
    return true;
}

bool ByteReader::u32(std::uint32_t& out) noexcept {
    return takeLe(data_, pos_, out);
}

bool ByteReader::u64(std::uint64_t& out) noexcept {
    return takeLe(data_, pos_, out);
}

bool ByteReader::i32(std::int32_t& out) noexcept {
    std::uint32_t raw = 0;
    if (!takeLe(data_, pos_, raw))
        return false;
    out = static_cast<std::int32_t>(raw);
    return true;
}

bool ByteReader::i64(std::int64_t& out) noexcept {
    std::uint64_t raw = 0;
    if (!takeLe(data_, pos_, raw))
        return false;
    out = static_cast<std::int64_t>(raw);
    return true;
}

bool ByteReader::f64(double& out) noexcept {
    std::uint64_t raw = 0;
    if (!takeLe(data_, pos_, raw))
        return false;
    out = std::bit_cast<double>(raw);
    return true;
}

bool ByteReader::varint(std::uint64_t& out) noexcept {
    out = 0;
    for (unsigned shift = 0; shift < 64u; shift += 7u) {
        if (pos_ >= data_.size())
            return false;
        const auto byte = static_cast<std::uint8_t>(data_[pos_++]);
        out |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0)
            return true;
    }
    return false;  // больше десяти байт — повреждённый поток
}

bool ByteReader::svarint(std::int64_t& out) noexcept {
    std::uint64_t raw = 0;
    if (!varint(raw))
        return false;
    out = unzigzag(raw);
    return true;
}

bool ByteReader::str(std::string& out) {
    std::uint64_t len = 0;
    if (!varint(len) || pos_ + len > data_.size())
        return false;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — std::string принимает const char*
    out.assign(reinterpret_cast<const char*>(data_.data() + pos_), len);
    pos_ += len;
    return true;
}

bool ByteReader::bytes(std::span<std::byte> out) noexcept {
    if (pos_ + out.size() > data_.size())
        return false;
    if (!out.empty())
        std::memcpy(out.data(), data_.data() + pos_, out.size());
    pos_ += out.size();
    return true;
}

bool ByteReader::array(std::span<std::uint32_t> out) noexcept {
    return readArray<std::uint32_t, std::uint32_t>(data_, pos_, out);
}

bool ByteReader::array(std::span<std::uint64_t> out) noexcept {
    return readArray<std::uint64_t, std::uint64_t>(data_, pos_, out);
}

bool ByteReader::array(std::span<double> out) noexcept {
    return readArray<double, std::uint64_t>(data_, pos_, out);
}

}  // namespace WaSafe
