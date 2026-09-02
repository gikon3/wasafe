#pragma once

#include <cstddef>
#include <vector>

#include "wasafe/export.hpp"
#include "wasafe/storage/block_source.hpp"

namespace WaSafe {

/// Источник, хранящий блоки в НАШЕЙ сериализации (см. encodeBlock/decodeBlock):
/// *.wsfstore на диске, буфер в ОЗУ, удалённый объект. Наследнику остаётся
/// прочитать stored-байты по смещению и распаковать их кодеком блока — разбор
/// в DecodedBlock этот слой берёт на себя.
///
/// Формату со СВОИМ блочным устройством промежуточный слой не нужен: он
/// наследуется прямо от BlockSource и собирает DecodedBlock из собственных
/// структур, без круга через наш формат.
class WASAFE_API RawBlockSource : public BlockSource {
public:
    ~RawBlockSource() override = default;

    /// Прочитать и распаковать блок. Возврат — распакованные байты длиной
    /// ref.rawSize. Об ошибках сообщает исключением домена.
    [[nodiscard]] virtual std::vector<std::byte> readBlock(const BlockRef& ref) const = 0;

    /// decodeBlock(readBlock(ref)). SignalId не используется: в нашем формате
    /// блок принадлежит ровно одному потоку и адресуется ссылкой полностью.
    [[nodiscard]] DecodedBlock decode(SignalId id, const BlockRef& ref) const override;

    /// Сверять ли прочитанный блок с тем, что о нём говорит индекс:
    /// контрольную сумму (BlockRef::crc32) и число изменений (BlockRef::count).
    ///
    /// Дубликат источника наследует этот флаг: иначе размноженная по потокам БД
    /// молча читала бы мимо проверки.
    void setVerifyChecksums(bool on) noexcept { verify_ = on; }
    [[nodiscard]] bool verifyChecksums() const noexcept { return verify_; }

protected:
    RawBlockSource() = default;
    RawBlockSource(const RawBlockSource&) = default;
    RawBlockSource(RawBlockSource&&) = default;

    RawBlockSource& operator=(const RawBlockSource&) = default;
    RawBlockSource& operator=(RawBlockSource&&) = default;

private:
    bool verify_ = false;
};

}  // namespace WaSafe
