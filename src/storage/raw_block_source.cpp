#include "wasafe/storage/raw_block_source.hpp"

#include <cstddef>
#include <vector>

#include "core/crc32.hpp"
#include "wasafe/core/exception.hpp"
#include "wasafe/storage/decoded_block.hpp"

namespace WaSafe {

DecodedBlock RawBlockSource::decode(SignalId, const BlockRef& ref) const {
    const std::vector<std::byte> raw = readBlock(ref);

    // Сумма считается ДО разбора: испорченные байты иначе дошли бы до декодера и
    // проявились бы случайной ошибкой формата вместо внятного «блок повреждён».
    if (verifyChecksums() && ref.crc32 != 0 && Crc32::compute(raw) != ref.crc32)
        throw Exception{"block: checksum mismatch"};

    DecodedBlock block = decodeBlock(raw);

    // А это уже про рассинхрон индекса и данных: сумма сошлась бы у обоих.
    if (verifyChecksums() && ref.count != 0 && block.count() != ref.count)
        throw Exception{"block: change count differs from index"};
    return block;
}

}  // namespace WaSafe
