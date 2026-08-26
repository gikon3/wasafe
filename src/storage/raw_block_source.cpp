#include "wasafe/storage/raw_block_source.hpp"

#include "wasafe/storage/decoded_block.hpp"

namespace WaSafe {

DecodedBlock RawBlockSource::decode(SignalId /*id*/, const BlockRef& ref) const {
    return decodeBlock(readBlock(ref));
}

}  // namespace WaSafe
