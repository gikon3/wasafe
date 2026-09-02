#include "wasafe/storage/signal_index.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>

#include "core/byte_io.hpp"

namespace WaSafe {

std::pair<std::size_t, std::size_t> SignalLocator::blocksIn(TimeRange range) const noexcept {
    // Блоки отсортированы по time.begin. Бинарным поиском находим первый блок,
    // который может пересекать range, затем линейно набираем пересекающиеся.
    const auto first = std::ranges::lower_bound(blocks, range.begin, std::less_equal{},
            [](const BlockRef& b) { return b.time.end; });
    auto last = first;
    while (last != blocks.end() && last->time.begin < range.end)
        ++last;
    return {static_cast<std::size_t>(first - blocks.begin()), static_cast<std::size_t>(last - blocks.begin())};
}

const SignalLocator* SignalIndex::locate(SignalId id) const noexcept {
    const auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : &it->second;
}

void SignalIndex::addBlock(SignalId id, BlockRef block) {
    streams_[id].blocks.push_back(block);  // упорядоченность поддерживает вызывающий
}

// ---------------------------------------------------------------------------
// Сериализация: тело секции 'WSFI' в файле store. Магию, версию и длину секции
// пишет вокруг этих байт слой раскладки (см. src/io/store_layout.hpp) — кодеку
// остаётся только содержимое.
// ---------------------------------------------------------------------------
void SignalIndex::encode(ByteWriter& w) const {
    w.i64(timeRange_.begin);
    w.i64(timeRange_.end);
    w.i32(timeScale_.exponent);
    w.i64(timeScale_.scale);
    w.u32(static_cast<std::uint32_t>(streams_.size()));

    for (const auto& [id, loc] : streams_) {
        w.u32(id.get());
        w.u32(static_cast<std::uint32_t>(loc.blocks.size()));
        for (const BlockRef& b : loc.blocks) {
            w.i64(b.time.begin);
            w.i64(b.time.end);
            w.u64(b.offset);
            w.u64(b.cookie);
            w.u32(b.storedSize);
            w.u32(b.rawSize);
            w.u32(b.crc32);
            w.u32(b.count);
            w.u8(static_cast<std::uint8_t>(b.codec));
        }
    }
}

SignalIndex SignalIndex::decode(ByteReader& r) {
    SignalIndex idx;
    TimeRange tr{};
    TimeScale ts{};
    if (!r.i64(tr.begin) || !r.i64(tr.end) || !r.i32(ts.exponent) || !r.i64(ts.scale))
        throw Exception{"index: truncated header"};
    idx.timeRange_ = tr;
    idx.timeScale_ = ts;

    std::uint32_t streamCount = 0;
    if (!r.u32(streamCount))
        throw Exception{"index: truncated stream count"};

    for (std::uint32_t s = 0; s < streamCount; ++s) {
        std::uint32_t rawId = 0;
        std::uint32_t blockCount = 0;
        if (!r.u32(rawId) || !r.u32(blockCount))
            throw Exception{"index: truncated stream header"};
        SignalLocator& loc = idx.streams_[SignalId{rawId}];
        loc.blocks.reserve(blockCount);
        for (std::uint32_t b = 0; b < blockCount; ++b) {
            BlockRef ref{};
            std::uint8_t codec = 0;
            if (!r.i64(ref.time.begin) || !r.i64(ref.time.end) || !r.u64(ref.offset) || !r.u64(ref.cookie) ||
                    !r.u32(ref.storedSize) || !r.u32(ref.rawSize) || !r.u32(ref.crc32) || !r.u32(ref.count) ||
                    !r.u8(codec))
                throw Exception{"index: truncated block"};
            ref.codec = static_cast<BlockRef::Codec>(codec);
            loc.blocks.push_back(ref);
        }
    }
    return idx;
}

}  // namespace WaSafe
