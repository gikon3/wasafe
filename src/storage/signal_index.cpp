#include "wasafe/storage/signal_index.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <span>

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
// Сериализация сайдкара (*.wsfidx). Хостовый порядок байт, версия 1
// (как и формат блока; TODO: переносимый little-endian).
// ---------------------------------------------------------------------------
namespace {

constexpr std::uint32_t kIndexMagic = 0x31584957u;  // 'WIX1'
constexpr std::uint32_t kIndexVersion = 1u;

template <class T>
void put(std::vector<std::byte>& out, const T& v) {
    const std::size_t off = out.size();
    out.resize(off + sizeof(T));
    std::memcpy(out.data() + off, &v, sizeof(T));
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data_(data) {}
    template <class T>
    [[nodiscard]] bool read(T& out) noexcept {
        if (pos_ + sizeof(T) > data_.size())
            return false;
        std::memcpy(&out, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }

private:
    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
};

}  // namespace

void SignalIndex::save(const std::filesystem::path& path) const {
    std::vector<std::byte> buf;
    put(buf, kIndexMagic);
    put(buf, kIndexVersion);
    put(buf, timeRange_.begin);
    put(buf, timeRange_.end);
    put(buf, timeScale_.exponent);
    put(buf, timeScale_.scale);
    put(buf, static_cast<std::uint32_t>(streams_.size()));

    for (const auto& [id, loc] : streams_) {
        put(buf, id.get());
        put(buf, static_cast<std::uint32_t>(loc.blocks.size()));
        for (const BlockRef& b : loc.blocks) {
            put(buf, b.time.begin);
            put(buf, b.time.end);
            put(buf, b.offset);
            put(buf, b.storedSize);
            put(buf, b.rawSize);
            put(buf, static_cast<std::uint8_t>(b.codec));
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw Exception{"cannot open index for write: " + path.string()};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — ostream::write требует const char*
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!out)
        throw Exception{"short write to index: " + path.string()};
}

SignalIndex SignalIndex::load(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in)
        throw Exception{"cannot open index: " + path.string()};
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<std::byte> buf(size);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — istream::read требует char*
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size))
        throw Exception{"index: short read"};

    Reader r(buf);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!r.read(magic) || magic != kIndexMagic)
        throw Exception{"index: bad magic"};
    if (!r.read(version) || version != kIndexVersion)
        throw Exception{"index: unsupported version"};

    SignalIndex idx;
    TimeRange tr{};
    TimeScale ts{};
    if (!r.read(tr.begin) || !r.read(tr.end) || !r.read(ts.exponent) || !r.read(ts.scale))
        throw Exception{"index: truncated header"};
    idx.timeRange_ = tr;
    idx.timeScale_ = ts;

    std::uint32_t streamCount = 0;
    if (!r.read(streamCount))
        throw Exception{"index: truncated stream count"};

    for (std::uint32_t s = 0; s < streamCount; ++s) {
        std::uint32_t rawId = 0;
        std::uint32_t blockCount = 0;
        if (!r.read(rawId) || !r.read(blockCount))
            throw Exception{"index: truncated stream header"};
        SignalLocator& loc = idx.streams_[SignalId{rawId}];
        loc.blocks.reserve(blockCount);
        for (std::uint32_t b = 0; b < blockCount; ++b) {
            BlockRef ref{};
            std::uint8_t codec = 0;
            if (!r.read(ref.time.begin) || !r.read(ref.time.end) || !r.read(ref.offset) || !r.read(ref.storedSize) ||
                    !r.read(ref.rawSize) || !r.read(codec))
                throw Exception{"index: truncated block"};
            ref.codec = static_cast<BlockRef::Codec>(codec);
            loc.blocks.push_back(ref);
        }
    }
    return idx;
}

}  // namespace WaSafe
