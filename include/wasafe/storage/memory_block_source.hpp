#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "wasafe/export.hpp"
#include "wasafe/storage/block_source.hpp"
#include "wasafe/storage/raw_block_source.hpp"

namespace WaSafe {

/// Источник блоков из ОЗУ: хранит stored-байты по ключу-смещению — ровно в том
/// виде, в каком они легли бы в файл, — и при чтении распаковывает их согласно
/// BlockRef::codec. Симметричен FileBlockSource: разница лишь в том, откуда
/// берутся байты (поиск в словаре против seek + read).
/// Удобен для тестов и для сборки ленивого хранилища без обращения к диску.
class WASAFE_API MemoryBlockSource final : public RawBlockSource {
public:
    /// Зарегистрировать stored-байты блока по его смещению: сжатые, если
    /// BlockRef этого блока объявляет кодек, иначе — как есть.
    void put(std::uint64_t offset, std::vector<std::byte> bytes);
    [[nodiscard]] std::vector<std::byte> readBlock(const BlockRef& block) const override;

private:
    std::unordered_map<std::uint64_t, std::vector<std::byte>> blocks_;
};

}  // namespace WaSafe
