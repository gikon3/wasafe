#include "wasafe/io/store.hpp"

#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/byte_io.hpp"
#include "io/hierarchy_codec.hpp"
#include "io/store_layout.hpp"
#include "wasafe/core/exception.hpp"
#include "wasafe/storage/file_block_source.hpp"
#include "wasafe/storage/lazy_storage.hpp"
#include "wasafe/storage/signal_index.hpp"

namespace WaSafe {

namespace {

/// Прочитать ровно n байт с заданного смещения.
std::vector<std::byte> readAt(std::ifstream& in, std::uint64_t offset, std::size_t n, std::string_view what) {
    std::vector<std::byte> buf(n);
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — istream::read требует char*
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
    if (in.gcount() != static_cast<std::streamsize>(n))
        throw Exception{std::string{"store: short read of "}.append(what)};
    return buf;
}

}  // namespace

Database openStore(const std::filesystem::path& path, LazyStorageOptions opts) {
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in)
        throw Exception{"cannot open store: " + path.string()};

    const auto size = static_cast<std::uint64_t>(in.tellg());
    if (size < StoreLayout::kHeaderSize + StoreLayout::kFooterSize)
        throw Exception{"store: file is too small to be a store: " + path.string()};

    // Заголовок: отличает «не тот файл» от «наш, но недописанный».
    {
        const auto raw = readAt(in, 0, StoreLayout::kHeaderSize, "header");
        ByteReader r{raw};
        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        if (!r.u32(magic) || !r.u32(version))
            throw Exception{"store: truncated header"};
        if (magic != StoreLayout::kMagic)
            throw Exception{"store: not a wasafe store: " + path.string()};
        if (version != StoreLayout::kVersion)
            throw Exception{"store: unsupported version: " + path.string()};
    }

    // Футер в самом конце — точка входа: по нему находятся метаданные.
    std::uint64_t metaOffset = 0;
    std::uint64_t metaSize = 0;
    {
        const auto raw = readAt(in, size - StoreLayout::kFooterSize, StoreLayout::kFooterSize, "footer");
        ByteReader r{raw};
        std::uint32_t magic = 0;
        if (!r.u64(metaOffset) || !r.u64(metaSize) || !r.u32(magic))
            throw Exception{"store: truncated footer"};
        if (magic != StoreLayout::kFooterMagic)
            throw Exception{"store: incomplete, ingestion did not finish: " + path.string()};
        if (metaOffset < StoreLayout::kHeaderSize || metaSize > size ||
                metaOffset + metaSize + StoreLayout::kFooterSize > size) {
            throw Exception{"store: metadata extent is out of file bounds: " + path.string()};
        }
    }

    const auto meta = readAt(in, metaOffset, static_cast<std::size_t>(metaSize), "metadata");
    ByteReader r{meta};
    Hierarchy hierarchy = decodeHierarchy(r);
    SignalIndex index = SignalIndex::decode(r);

    auto source = FileBlockSource::open(path);
    return Database{std::move(hierarchy), std::make_unique<LazyStorage>(std::move(index), std::move(source), opts)};
}

}  // namespace WaSafe
