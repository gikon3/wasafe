#include "wasafe/storage/file_block_source.hpp"

#include "storage/decompress.hpp"
#include "wasafe/core/exception.hpp"

namespace WaSafe {

std::unique_ptr<FileBlockSource> FileBlockSource::open(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file)
        throw Exception{"cannot open " + path.string()};
    // Конструктор приватный — используем new через обёртку.
    return std::unique_ptr<FileBlockSource>(new FileBlockSource(path, std::move(file)));
}

std::unique_ptr<BlockSource> FileBlockSource::duplicate() const {
    auto copy = open(path_);  // новый дескриптор со своей позицией, а не копия ifstream
    copy->setVerifyChecksums(verifyChecksums());
    return copy;
}

std::vector<std::byte> FileBlockSource::readBlock(const BlockRef& block) const {
    std::vector<std::byte> stored(block.storedSize);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(block.offset));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — istream::read требует char*
    file_.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(block.storedSize));
    if (file_.gcount() != static_cast<std::streamsize>(block.storedSize))
        throw Exception{"FileBlockSource: short read"};
    return decompress(std::move(stored), block.codec, block.rawSize);
}

}  // namespace WaSafe
