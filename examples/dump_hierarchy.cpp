// Пример: разобрать источник диаграммы и распечатать иерархию, демонстрируя
// ЕДИНЫЙ обход сигналов независимо от типа и уровня вложенности (шины,
// структуры, массивы).
//
// Источник — DemoReader: библиотека не содержит парсеров форматов, нужный
// Reader выбирает пользователь и передаёт в ingest() вместе с приёмником.

#include <cstddef>
#include <print>
#include <string>

#include "demo_reader.hpp"
#include "wasafe/wasafe.hpp"

namespace {

// Рекурсивный обход одного сигнала: одинаков для скаляра, шины, структуры,
// объединения и многомерного массива — это и есть единый интерфейс.
// NOLINTNEXTLINE(misc-no-recursion)
void dumpSignal(const WaSafe::Signal& sig, int indent) {
    const std::string pad(static_cast<size_t>(indent * 2), ' ');
    std::println("{}{} : {}", pad, sig.name(), WaSafe::toSvString(sig.type()));
    for (const WaSafe::Signal child : sig.children()) {
        dumpSignal(child, indent + 1);  // члены структуры / элементы массива
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
void dumpScope(const WaSafe::Scope& scope, int indent) {
    const std::string pad(static_cast<size_t>(indent * 2), ' ');
    std::println("{}[{}] {}", pad, WaSafe::toString(scope.kind()), scope.name());
    for (const WaSafe::Signal sig : scope.signals()) {
        dumpSignal(sig, indent + 1);
    }
    for (const WaSafe::Scope child : scope.scopes()) {
        dumpScope(child, indent + 1);
    }
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    try {
        // Reader (какой именно — решает пользователь) + приёмник в ОЗУ.
        Demo::DemoReader reader;
        auto sink = WaSafe::makeMemoryBuilder();
        auto db = WaSafe::ingest(reader, *sink);

        std::println("timescale: {}", WaSafe::formatTimeScale(db.timeScale()));
        const auto tr = db.timeRange();
        std::println("time range: [{}, {})", tr.begin, tr.end);
        dumpScope(db.root(), 0);
    }
    catch (const WaSafe::Exception& e) {
        std::println(stderr, "error: {}", e.what());
        return 1;
    }
    return 0;
}
