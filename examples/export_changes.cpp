// Пример: ЭКСПОРТ всей диаграммы — все изменения дизайна в общем
// хронологическом порядке, одним проходом.
//
// Это скелет любого Writer'а (VCD, FST, CSV): обойти иерархию и присвоить
// каждому листу собственный идентификатор, затем открыть ОДИН курсор сразу по
// всем листьям. Курсор сливает потоки по времени и в поле source сообщает,
// чей это лист — индекс в том самом списке, который был передан в changes().
// Своими руками мерджить курсоры листьев внешнему проекту не нужно.

#include <cstdint>
#include <print>
#include <string>
#include <vector>

#include "demo_reader.hpp"
#include "wasafe/wasafe.hpp"

namespace {

/// Короткий печатный идентификатор потока — как коды VCD ('!', '"', '#', ...).
std::string identOf(std::size_t index) {
    return std::string{static_cast<char>('!' + (index % 94))};
}

std::string toText(WaSafe::ValueView v) {
    switch (v.kind()) {
        case WaSafe::ValueKind::LOGIC:
            return "b" + v.logic().toString();
        case WaSafe::ValueKind::REAL:
            return "r" + std::to_string(v.real());
        case WaSafe::ValueKind::STRING:
            return "s" + std::string{v.string()};
        default:
            return "<none>";
    }
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    try {
        Demo::DemoReader reader;
        auto sink = WaSafe::makeMemoryBuilder();
        auto db = WaSafe::ingest(reader, *sink);

        // Фаза объявлений: все листовые узлы дизайна, обходом в глубину.
        // Packed-структура — один лист: наружу она отдаётся одним потоком, и её
        // члены восстанавливаются из битовых срезов.
        const std::vector<WaSafe::NodeId> leaves = db.leafNodes(db.root().id());

        std::println("$scope design $end");
        for (std::size_t i = 0; i < leaves.size(); ++i) {
            const WaSafe::Signal sig = db.signalHandle(leaves[i]);
            std::println("$var {} {} {} $end", identOf(i), sig.width(), sig.fullPath());
        }
        std::println("$enddefinitions $end");

        // Фаза значений: ОДИН курсор по всем листьям сразу вместо N независимых.
        // source — индекс листа в leaves, поэтому таблица идентификаторов
        // индексируется напрямую, без поиска по SignalId.
        auto cursor = db.changes(leaves, db.timeRange());
        WaSafe::TimeStamp lastTime = WaSafe::kNoTime;
        std::size_t count = 0;
        while (cursor.next()) {
            if (cursor->time != lastTime) {  // метка времени печатается один раз на момент
                std::println("#{}", cursor->time);
                lastTime = cursor->time;
            }
            std::println("{} {}", toText(cursor->value), identOf(cursor->source));
            ++count;
        }

        std::println("$comment {} изменений, {} сигналов $end", count, leaves.size());
    }
    catch (const WaSafe::Exception& e) {
        std::println(stderr, "error: {}", e.what());
        return 1;
    }
    return 0;
}
