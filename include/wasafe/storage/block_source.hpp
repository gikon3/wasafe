#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wasafe/core/time.hpp"
#include "wasafe/export.hpp"

namespace WaSafe {

/// Дескриптор блока значений в файле/нормализованном хранилище.
/// Блок покрывает диапазон времени и хранит изменения одного или нескольких
/// потоков; адресуется смещением в файле. Это основа ленивой подгрузки.
struct BlockRef {
    TimeRange time;                ///< покрываемый диапазон времени
    std::uint64_t offset = 0;      ///< смещение в файле/хранилище
    std::uint32_t storedSize = 0;  ///< размер на диске (возможно, сжатый)
    std::uint32_t rawSize = 0;     ///< размер после распаковки
    enum class Codec : std::uint8_t { NONE, ZSTD, LZ4, ZLIB } codec = Codec::NONE;
};

/// Источник сырых блоков. Абстрагирует «откуда читать байты»: файл VCD/FST,
/// нормализованный *.wsfstore, удалённый объект и т.п. Реализация отвечает за
/// чтение по смещению и распаковку согласно codec блока.
class WASAFE_API BlockSource {
public:
    virtual ~BlockSource() = default;

    /// Прочитать и распаковать блок. Возврат — распакованные байты длиной
    /// block.raw_size. Может бросать/возвращать ошибку через исключение домена.
    [[nodiscard]] virtual std::vector<std::byte> readBlock(const BlockRef& block) const = 0;

protected:
    BlockSource() = default;
    BlockSource(const BlockSource&) = default;
    BlockSource(BlockSource&&) = default;

    BlockSource& operator=(const BlockSource&) = default;
    BlockSource& operator=(BlockSource&&) = default;
};

}  // namespace WaSafe
