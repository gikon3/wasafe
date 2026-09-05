#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "wasafe/export.hpp"
#include "wasafe/storage/block_source.hpp"
#include "wasafe/storage/raw_block_source.hpp"
#include "wasafe/storage/signal_index.hpp"

namespace WaSafe {

/// Источник блоков из файла на диске. Читает stored_size байт по смещению и
/// распаковывает их кодеком блока до raw_size. Не потокобезопасен: один источник
/// рассчитан на использование из одного потока (внутренний std::ifstream).
/// Второму потоку — свой источник через duplicate().
class WASAFE_API FileBlockSource final : public RawBlockSource {
public:
    [[nodiscard]] static std::unique_ptr<FileBlockSource> open(const std::filesystem::path& path);
    [[nodiscard]] std::vector<std::byte> readBlock(const BlockRef& block) const override;
    /// Тот же файл, открытый заново: свой дескриптор со своей позицией.
    [[nodiscard]] std::unique_ptr<BlockSource> duplicate() const override;

private:
    FileBlockSource(std::filesystem::path path, std::ifstream file) : path_{std::move(path)}, file_{std::move(file)} {}

private:
    std::filesystem::path path_;  ///< нужен duplicate(): ifstream не копируется
    mutable std::ifstream file_;  ///< mutable: чтение по смещению в const-методе
};

}  // namespace WaSafe
