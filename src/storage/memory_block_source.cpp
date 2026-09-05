#include "wasafe/storage/memory_block_source.hpp"

#include "storage/decompress.hpp"
#include "wasafe/core/exception.hpp"

namespace WaSafe {

void MemoryBlockSource::put(std::uint64_t offset, std::vector<std::byte> bytes) {
    (*blocks_)[offset] = std::move(bytes);
}

std::vector<std::byte> MemoryBlockSource::readBlock(const BlockRef& block) const {
    const auto it = blocks_->find(block.offset);
    if (it == blocks_->end())
        throw Exception{"MemoryBlockSource: no block at given offset"};
    return decompress(it->second, block.codec, block.rawSize);
}

std::unique_ptr<BlockSource> MemoryBlockSource::duplicate() const {
    auto copy = std::make_unique<MemoryBlockSource>();
    copy->blocks_ = blocks_;  // байты разделяются, копировать их незачем
    copy->setVerifyChecksums(verifyChecksums());
    return copy;
}

}  // namespace WaSafe
