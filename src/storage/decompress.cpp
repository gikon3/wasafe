#include "decompress.hpp"

#include "wasafe/config.hpp"
#include "wasafe/core/exception.hpp"

#if WASAFE_HAS_ZSTD
#include <zstd.h>
#endif

namespace WaSafe {

std::vector<std::byte> decompress(std::vector<std::byte> stored, BlockRef::Codec codec, std::uint32_t rawSize) {
    switch (codec) {
        case BlockRef::Codec::NONE:
            return stored;  // уже сырые байты

        case BlockRef::Codec::ZSTD: {
#if WASAFE_HAS_ZSTD
            std::vector<std::byte> out(rawSize);
            const std::size_t n = ZSTD_decompress(out.data(), out.size(), stored.data(), stored.size());
            if (ZSTD_isError(n) || n != rawSize) {
                throw Exception("block_source: zstd decompress failed");
            }
            return out;
#else
            throw Exception("block_source: zstd codec not built");
#endif
        }

        case BlockRef::Codec::LZ4:
        case BlockRef::Codec::ZLIB:
            // TODO(impl): подключить lz4/zlib (используются плагином FST).
            throw Exception("block_source: codec not supported in core");
    }
    throw Exception("block_source: unknown codec");
}

}  // namespace WaSafe
