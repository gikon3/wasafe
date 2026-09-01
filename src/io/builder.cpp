#include "wasafe/io/builder.hpp"

#include "io/indexing_builder.hpp"
#include "io/memory_builder.hpp"
#include "io/validating_builder.hpp"

namespace WaSafe {

std::unique_ptr<Builder> makeMemoryBuilder() {
    return std::make_unique<MemoryBuilder>();
}

std::unique_ptr<Builder> makeIndexingBuilder(const std::filesystem::path& storePath, IndexingOptions opts) {
    return std::make_unique<IndexingBuilder>(storePath, opts);
}

std::unique_ptr<Builder> makeValidatingBuilder(Builder& sink) {
    return std::make_unique<ValidatingBuilder>(sink);
}

}  // namespace WaSafe
