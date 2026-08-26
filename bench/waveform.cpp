#include "waveform.hpp"

#include <array>
#include <format>
#include <string>

#include "wasafe/types/logic_vector.hpp"
#include "wasafe/types/type.hpp"

namespace Bench {

namespace {

using namespace WaSafe;

/// Периоды переключения и доли потоков на каждом. Клоки — 2% сигналов, но
/// больше половины всех изменений: ровно так устроен и настоящий дамп.
constexpr std::array<std::uint32_t, 4> kPeriods{2, 8, 64, 1024};
constexpr std::array<std::uint32_t, 4> kShare{2, 8, 30, 60};  // проценты, в сумме 100

/// Ширины logic-потоков по кругу.
constexpr std::array<std::uint32_t, 5> kWidths{1, 8, 16, 32, 64};

/// Быстрый детерминированный генератор: обычный mt19937 сам стал бы заметной
/// долей замера.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept : state_{seed | 1u} {}

    std::uint64_t next() noexcept {
        state_ ^= state_ << 13u;
        state_ ^= state_ >> 7u;
        state_ ^= state_ << 17u;
        return state_;
    }

private:
    std::uint64_t state_;
};

struct StreamInfo {
    SignalId id;
    std::uint32_t width = 0;  ///< 0 у real/string
    std::uint32_t period = 0;
    ValueKind kind = ValueKind::LOGIC;
};

/// Индекс группы периодов для потока i — по накопленным долям.
std::size_t periodBucket(std::uint32_t i, std::uint32_t total) {
    const std::uint32_t pos = total == 0 ? 0 : (i * 100u) / total;
    std::uint32_t acc = 0;
    for (std::size_t b = 0; b < kShare.size(); ++b) {
        acc += kShare[b];
        if (pos < acc)
            return b;
    }
    return kShare.size() - 1;
}

}  // namespace

std::uint64_t estimateChanges(const Spec& spec) {
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < spec.streams; ++i) {
        const std::uint32_t period = kPeriods[periodBucket(i, spec.streams)];
        total += static_cast<std::uint64_t>(spec.endTime) / period;
    }
    return total;
}

Stats generate(Builder& sink, const Spec& spec) {
    sink.setTimeScale({.exponent = -12, .scale = 1});
    sink.beginScope("top", ScopeKind::MODULE);

    std::vector<StreamInfo> streams;
    streams.reserve(spec.streams);

    std::uint32_t inScope = 0;
    std::uint32_t scopeIndex = 0;
    sink.beginScope(std::format("u_mod{}", scopeIndex), ScopeKind::MODULE);

    for (std::uint32_t i = 0; i < spec.streams; ++i) {
        if (inScope == spec.signalsPerScope) {
            sink.endScope();
            sink.beginScope(std::format("u_mod{}", ++scopeIndex), ScopeKind::MODULE);
            inScope = 0;
        }
        ++inScope;

        StreamInfo info;
        info.period = kPeriods[periodBucket(i, spec.streams)];

        if (i % 97 == 96) {  // редкие вещественные
            info.kind = ValueKind::REAL;
            info.id = sink.declareVar(std::format("f{}", i), makeReal());
        }
        else if (i % 193 == 192) {  // ещё более редкие строковые
            info.kind = ValueKind::STRING;
            info.id = sink.declareVar(std::format("s{}", i), makeString());
        }
        else {
            info.width = kWidths[i % kWidths.size()];
            info.kind = ValueKind::LOGIC;
            const Type t = info.width == 1 ? makeScalar() : makeVector(static_cast<std::int32_t>(info.width) - 1, 0);
            info.id = sink.declareVar(std::format("sig{}", i), t);
        }
        streams.push_back(info);
    }

    sink.endScope();  // последний модуль
    sink.endScope();  // top
    sink.headerDone();

    // Раскладка потоков по группам периодов: на каждом тике трогаются только те
    // группы, чей период его делит.
    std::array<std::vector<std::uint32_t>, kPeriods.size()> byPeriod;
    for (std::uint32_t i = 0; i < streams.size(); ++i) {
        for (std::size_t b = 0; b < kPeriods.size(); ++b) {
            if (streams[i].period == kPeriods[b]) {
                byPeriod[b].push_back(i);
                break;
            }
        }
    }

    // Скретчи на каждую ширину: приёмник копирует значение немедленно, поэтому
    // буфер переиспользуется — как и положено парсеру.
    std::array<LogicVector, kWidths.size()> scratch{LogicVector{kWidths[0]}, LogicVector{kWidths[1]},
            LogicVector{kWidths[2]}, LogicVector{kWidths[3]}, LogicVector{kWidths[4]}};

    Rng rng{spec.seed};
    Stats stats;
    stats.streams = spec.streams;

    std::string strScratch;
    for (TimeStamp t = 0; t < spec.endTime; t += kPeriods[0]) {
        bool timeSet = false;
        for (std::size_t b = 0; b < kPeriods.size(); ++b) {
            if (t % kPeriods[b] != 0)
                continue;
            for (const std::uint32_t si : byPeriod[b]) {
                if (!timeSet) {
                    sink.setTime(t);
                    timeSet = true;
                }
                const StreamInfo& info = streams[si];
                switch (info.kind) {
                    case ValueKind::REAL:
                        sink.valueChange(info.id, ValueView{static_cast<double>(rng.next() % 100000) / 16.0});
                        break;
                    case ValueKind::STRING:
                        strScratch = std::format("st{}", rng.next() % 1000);
                        sink.valueChange(info.id, ValueView{std::string_view{strScratch}});
                        break;
                    default: {
                        LogicVector& v = scratch[info.width == 1 ? 0
                                                                 : (info.width == 8                   ? 1
                                                                                   : info.width == 16 ? 2
                                                                                   : info.width == 32 ? 3
                                                                                                      : 4)];
                        const std::uint64_t bits = rng.next();
                        for (std::uint32_t bit = 0; bit < info.width; ++bit) {
                            // Раз в 64 изменения подмешиваем x/z: четырёхзначный
                            // путь кодировщика должен быть под нагрузкой тоже.
                            const bool unknown = (bits & 0x3Fu) == 0 && bit == 0;
                            v.set(bit, unknown ? Logic::X : ((bits >> (bit % 64u)) & 1u ? Logic::ONE : Logic::ZERO));
                        }
                        sink.valueChange(info.id, ValueView{v});
                        break;
                    }
                }
                ++stats.changes;
            }
        }
    }
    return stats;
}

}  // namespace Bench
