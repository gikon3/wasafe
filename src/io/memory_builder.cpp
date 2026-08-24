#include "memory_builder.hpp"

#include "wasafe/storage/database.hpp"

namespace WaSafe {

void MemoryBuilder::valueChange(SignalId id, ValueView value) {
    backend_.append(id, now_, value);
}

void MemoryBuilder::finish() {
    backend_.setTimeScale(scale_);
    backend_.finalize();
}

Database MemoryBuilder::takeDatabase() {
    return Database{std::move(hierarchy_), std::make_unique<MemoryStorage>(std::move(backend_))};
}

SignalId MemoryBuilder::allocStream(ValueKind kind, std::uint32_t width) {
    return backend_.createStream(kind, width);
}

}  // namespace WaSafe
