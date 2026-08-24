#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "wasafe/export.hpp"
#include "wasafe/storage/block_source.hpp"

namespace WaSafe {

/// Источник блоков из ОЗУ: хранит уже распакованные байты, адресуемые смещением.
/// Удобен для тестов и для сборки ленивого хранилища без обращения к диску.
/// Ожидает блоки с codec == None (распаковка не выполняется).
class WASAFE_API MemoryBlockSource final : public BlockSource {
public:
    /// Зарегистрировать содержимое блока по его смещению.
    void put(std::uint64_t offset, std::vector<std::byte> bytes);
    [[nodiscard]] std::vector<std::byte> readBlock(const BlockRef& block) const override;

private:
    std::unordered_map<std::uint64_t, std::vector<std::byte>> blocks_;
};

}  // namespace WaSafe
