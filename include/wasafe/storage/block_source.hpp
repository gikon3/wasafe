#pragma once

#include <cstdint>

#include "wasafe/core/ids.hpp"
#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"

namespace WaSafe {

class DecodedBlock;

/// Дескриптор блока значений в файле/нормализованном хранилище.
/// Блок покрывает диапазон времени и хранит изменения одного или нескольких
/// потоков; адресуется смещением в файле. Это основа ленивой подгрузки.
struct BlockRef {
    TimeRange time;            ///< покрываемый диапазон времени
    std::uint64_t offset = 0;  ///< смещение в файле/хранилище
    /// Доадресация внутри offset для форматов, где один чанк несёт несколько
    /// потоков (FST): ядро значение НЕ интерпретирует, но учитывает в ключе
    /// кэша. Слот на 64 бита; контекст крупнее источник держит у себя, ключуя
    /// парой (SignalId, offset) — id приходит параметром decode().
    std::uint64_t cookie = 0;
    std::uint32_t storedSize = 0;  ///< размер на диске (возможно, сжатый)
    std::uint32_t rawSize = 0;     ///< размер после распаковки
    enum class Codec : std::uint8_t { NONE, ZSTD, LZ4, ZLIB } codec = Codec::NONE;
};

/// Источник блоков ленивого хранилища: абстрагирует «откуда берётся блок».
/// Реализации — нормализованный *.wsfstore, буфер в ОЗУ, файл чужого формата со
/// своим блочным устройством. LazyStorage знает только этот контракт и о том,
/// как блок уложен на носителе, не осведомлён.
class WASAFE_API BlockSource {
public:
    virtual ~BlockSource() = default;

    /// Блок потока id, описанный ссылкой ref.
    ///
    /// id — идентичность потока: по паре (id, ref.offset) источник находит у
    /// себя контекст блока любого размера. Она нужна форматам, где один чанк
    /// несёт изменения нескольких потоков и offset у их блоков общий; то, что
    /// влезает в 64 бита, можно вместо этого возить в ref.cookie.
    [[nodiscard]] virtual DecodedBlock decode(SignalId id, const BlockRef& ref) const = 0;

protected:
    BlockSource() = default;
    BlockSource(const BlockSource&) = default;
    BlockSource(BlockSource&&) = default;

    BlockSource& operator=(const BlockSource&) = default;
    BlockSource& operator=(BlockSource&&) = default;
};

}  // namespace WaSafe
