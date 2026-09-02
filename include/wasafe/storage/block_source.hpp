#pragma once

#include <cstdint>
#include <memory>

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
    /// Контрольная сумма РАСПАКОВАННОГО содержимого блока. Поле необязательное,
    /// и НОЛЬ означает «не задано»: заполняет его тот, кто пишет наш store, а
    /// источник чужого формата (FST) суммы не считает.
    std::uint32_t crc32 = 0;
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

    /// Второй НЕЗАВИСИМЫЙ доступ к тем же данным — как dup(2) для дескриптора.
    /// Состояние НЕ копируется: FileBlockSource открывает файл заново, а не
    /// дублирует позицию своего ifstream. Нужно, чтобы БД, размноженная по
    /// потокам, читала байты без всякой синхронизации; неизменяемое (блоки в
    /// ОЗУ) при этом разделяется.
    ///
    /// nullptr по умолчанию — источник размножения не поддерживает, и БД поверх
    /// него не дублируется (Database::duplicate() сообщит об этом исключением).
    [[nodiscard]] virtual std::unique_ptr<BlockSource> duplicate() const { return nullptr; }

protected:
    BlockSource() = default;
    BlockSource(const BlockSource&) = default;
    BlockSource(BlockSource&&) = default;

    BlockSource& operator=(const BlockSource&) = default;
    BlockSource& operator=(BlockSource&&) = default;
};

}  // namespace WaSafe
