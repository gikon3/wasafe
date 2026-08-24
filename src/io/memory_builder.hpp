#pragma once

#include "io/base_builder.hpp"
#include "wasafe/storage/memory_storage.hpp"

namespace WaSafe {

/// Builder, собирающий БД целиком в ОЗУ (поверх MemoryStorage).
class MemoryBuilder final : public BaseBuilder {
public:
    void valueChange(SignalId id, ValueView value) override;
    void finish() override;
    [[nodiscard]] Database takeDatabase() override;

protected:
    SignalId allocStream(ValueKind kind, std::uint32_t width) override;

private:
    MemoryStorage backend_;
};

}  // namespace WaSafe
