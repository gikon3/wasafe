#include "io/indexing_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "core/byte_io.hpp"
#include "core/crc32.hpp"
#include "io/hierarchy_codec.hpp"
#include "io/store_layout.hpp"
#include "wasafe/config.hpp"
#include "wasafe/storage/database.hpp"
#include "wasafe/storage/file_block_source.hpp"
#include "wasafe/storage/lazy_storage.hpp"

#if WASAFE_HAS_ZSTD
#include <zstd.h>
#endif

namespace WaSafe {

namespace {

/// Сжать сырые байты блока кодеком по умолчанию (zstd, если собран). Возвращает
/// (байты, codec). При невозможности/неэффективности сжатия — без изменений.
std::pair<std::vector<std::byte>, BlockRef::Codec> compressBlock(std::vector<std::byte> raw) {
#if WASAFE_HAS_ZSTD
    const std::size_t bound = ZSTD_compressBound(raw.size());
    std::vector<std::byte> out(bound);
    const std::size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), ZSTD_CLEVEL_DEFAULT);
    if (!ZSTD_isError(n) && n < raw.size()) {
        out.resize(n);
        return {std::move(out), BlockRef::Codec::ZSTD};
    }
#endif
    return {std::move(raw), BlockRef::Codec::NONE};
}

/// Объём полезных данных накопителя, без константной части самого объекта.
/// Учёт бюджета ведётся именно им: при append дельта byteSize() эту константу
/// сокращает, поэтому и вычитать при сбросе нужно без неё.
std::size_t payloadBytes(const DecodedBlock& b) noexcept {
    return b.byteSize() - sizeof(DecodedBlock);
}

}  // namespace

IndexingBuilder::IndexingBuilder(std::filesystem::path store, IndexingOptions opts) :
        store_{std::move(store)}, blockChanges_{opts.blockChanges ? opts.blockChanges : kDefaultBlockChanges},
        bufferBytes_{opts.bufferBytes ? opts.bufferBytes : kDefaultBufferBytes},
        out_{store_, std::ios::binary | std::ios::trunc} {
    // Поток открывается сразу: запись идёт по мере разбора, а не в finish().
    // Цена — прерванная ingestion оставляет файл без футера; открыть его нельзя,
    // но по заголовку видно, что это именно недописанный store.
    if (!out_)
        throw Exception{"cannot open store: " + store_.string()};

    std::vector<std::byte> header;
    ByteWriter w{header};
    w.u32(StoreLayout::kMagic);
    w.u32(StoreLayout::kVersion);
    writeRaw(header);
    offset_ = StoreLayout::kHeaderSize;  // блоки начинаются сразу за заголовком
}

void IndexingBuilder::writeRaw(std::span<const std::byte> data) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — ostream::write требует const char*
    out_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out_)
        throw Exception{"store: write failed: " + store_.string()};
    offset_ += data.size();
}

void IndexingBuilder::writeTrailer() {
    // Метаданные: иерархия и геометрия блоков. Без них файл не открыть, поэтому
    // ошибка записи фатальна.
    //
    // Каждая часть пишется отдельной секцией со своими магией, версией и длиной.
    // Тело собирается в свой буфер, потому что длина известна только после
    // записи, а стоит это ровно одной лишней копии метаданных в ОЗУ — они и так
    // целиком в памяти.
    std::vector<std::byte> hierarchy;
    ByteWriter hw{hierarchy};
    encodeHierarchy(hw, hierarchy_);

    std::vector<std::byte> index;
    ByteWriter iw{index};
    index_.encode(iw);

    std::vector<std::byte> meta;
    ByteWriter mw{meta};
    StoreLayout::writeSection(mw, StoreLayout::kHierarchyMagic, StoreLayout::kHierarchyVersion, hierarchy);
    StoreLayout::writeSection(mw, StoreLayout::kIndexMagic, StoreLayout::kIndexVersion, index);

    const std::uint64_t metaOffset = offset_;
    writeRaw(meta);

    std::vector<std::byte> footer;
    ByteWriter fw{footer};
    fw.u64(metaOffset);
    fw.u64(static_cast<std::uint64_t>(meta.size()));
    fw.u32(Crc32::compute(meta));  // тихое повреждение метаданных отличимо от обрыва записи
    fw.u32(StoreLayout::kFooterMagic);
    writeRaw(footer);
}

void IndexingBuilder::valueChange(SignalId id, ValueView value) {
    if (!id.valid() || id.get() >= streams_.size())
        return;

    Stream& s = streams_[id.get()];

    // byteSize() — O(1), поэтому учёт занятой памяти ведём по дельте.
    const std::size_t before = payloadBytes(s.block);
    s.block.append(now_, value);
    buffered_ += payloadBytes(s.block) - before;

    if (firstTime_ == kNoTime)
        firstTime_ = now_;
    lastTime_ = now_;

    if (s.block.count() >= blockChanges_)
        flushStream(id.get());
    else if (buffered_ > bufferBytes_)
        relieve();
}

void IndexingBuilder::flushStream(std::uint32_t sid) {
    Stream& s = streams_[sid];
    if (s.block.empty())
        return;

    const auto times = s.block.times();
    // Блок покрывает [первое изменение, последнее + 1). Между блоками одного
    // потока остаются «дыры»: время следующего изменения на момент сброса ещё
    // неизвестно. Читающая сторона это допускает — поиск блока идёт по
    // time.begin (см. LazyStorage::valueAt / SignalLocator::blocksIn).
    const TimeRange span{times.front(), times.back() + 1};

    std::vector<std::byte> raw = encodeBlock(s.block);
    const auto rawSize = static_cast<std::uint32_t>(raw.size());
    // Сумма считается по РАСПАКОВАННЫМ байтам: так она не зависит от того, каким
    // кодеком блок уляжется на диск, и проверяется ровно там, где данные уже
    // готовы к разбору.
    const std::uint32_t crc = Crc32::compute(raw);
    auto [stored, codec] = compressBlock(std::move(raw));

    index_.addBlock(SignalId{sid},
            BlockRef{.time = span,
                    .offset = offset_,
                    .storedSize = static_cast<std::uint32_t>(stored.size()),
                    .rawSize = rawSize,
                    .crc32 = crc,
                    .count = static_cast<std::uint32_t>(s.block.count()),
                    .codec = codec});

    writeRaw(stored);  // сам двигает offset_ и проверяет поток

    buffered_ -= std::min(buffered_, payloadBytes(s.block));
    s.block = DecodedBlock{s.kind, s.width};  // память накопителя возвращается
}

void IndexingBuilder::relieve() {
    const std::size_t target = bufferBytes_ / 2;

    std::size_t maxCount = 0;
    for (const Stream& s : streams_)
        maxCount = std::max(maxCount, s.block.count());
    if (maxCount == 0)
        return;

    // Полные проходы с убывающим порогом: сначала сбрасываем только заметно
    // наполненные накопители, и лишь если этого не хватило — всё подряд.
    // Проход целиком (а не до первого облегчения) убирает перекос в сторону
    // младших SignalId. Стартовый порог ограничен самым крупным накопителем,
    // иначе при большом blockChanges первые проходы были бы холостыми.
    for (std::size_t minCount = std::min(std::max<std::size_t>(blockChanges_ / 4, 1), maxCount);;) {
        for (std::uint32_t sid = 0; sid < streams_.size(); ++sid) {
            if (streams_[sid].block.count() >= minCount)
                flushStream(sid);
        }
        if (buffered_ <= target || minCount == 1)
            return;
        minCount = std::max<std::size_t>(minCount / 4, 1);
    }
}

void IndexingBuilder::finish() {
    if (finished_)
        return;
    finished_ = true;

    for (std::uint32_t sid = 0; sid < streams_.size(); ++sid)
        flushStream(sid);

    index_.setTimeScale(scale_);
    index_.setTimeRange(firstTime_ == kNoTime ? TimeRange{} : TimeRange{firstTime_, lastTime_ + 1});

    writeTrailer();

    out_.flush();
    if (!out_)
        throw Exception{"store flush failed"};
    out_.close();  // до того, как takeDatabase() откроет store на чтение
}

Database IndexingBuilder::takeDatabase() {
    auto src = FileBlockSource::open(store_);
    return Database{std::move(hierarchy_), std::make_unique<LazyStorage>(std::move(index_), std::move(src))};
}

SignalId IndexingBuilder::allocStream(ValueKind kind, std::uint32_t width) {
    const auto id = SignalId{static_cast<std::uint32_t>(streams_.size())};
    streams_.push_back(Stream{DecodedBlock{kind, width}, kind, width});
    return id;
}

}  // namespace WaSafe
