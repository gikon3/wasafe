#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wasafe/storage/block_source.hpp"

namespace WaSafe {

/// Распаковать stored-байты согласно кодеку до ожидаемого raw_size.
std::vector<std::byte> decompress(std::vector<std::byte> stored, BlockRef::Codec codec, std::uint32_t rawSize);

}  // namespace WaSafe
