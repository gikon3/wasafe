// Пример: ленивое чтение значений сигнала по ИМЕНИ и ВРЕМЕННОМУ ДИАПАЗОНУ.
// Источник (DemoReader) разбирается в нормализованный store на диске, после
// чего значения подгружаются из него поблочно, без загрузки всей диаграммы
// в память. Режим задаёт выбранный приёмник — indexing-builder.

#include <cstdlib>
#include <filesystem>
#include <print>
#include <system_error>

#include "demo_reader.hpp"
#include "wasafe/wasafe.hpp"

namespace {

// NOLINTNEXTLINE(misc-no-recursion)
void printValue(const WaSafe::Value& v) {
    switch (v.kind()) {
        case WaSafe::ValueKind::LOGIC:
            std::print("{}", v.asLogic().toString());
            break;
        case WaSafe::ValueKind::REAL:
            std::print("{}", v.asReal());
            break;
        case WaSafe::ValueKind::STRING:
            std::print("\"{}\"", v.asString());
            break;
        case WaSafe::ValueKind::AGGREGATE: {
            std::print("{{");
            bool first = true;
            for (const auto& e : v.asAggregate()) {
                if (!first)
                    std::print(", ");
                printValue(e);
                first = false;
            }
            std::print("}}");
            break;
        }
        default:
            std::print("<none>");
    }
}

/// Временный store: удаляется вместе с сайдкаром при выходе из main.
struct TempStore {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "wasafe_query_range.wsfstore";

    TempStore() = default;
    TempStore(const TempStore&) = delete;
    TempStore(TempStore&&) = delete;
    ~TempStore() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    TempStore& operator=(const TempStore&) = delete;
    TempStore& operator=(TempStore&&) = delete;
};

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    if (argc < 4) {
        std::println(stderr, "usage: query_range <signal.path> <begin> <end>   (напр. top.data 0 100)");
        return 2;
    }
    try {
        // Разбор источника в нормализованный store, затем ленивое чтение из него.
        const TempStore store;
        Demo::DemoReader reader;
        auto sink = WaSafe::makeIndexingBuilder(store.path);
        auto db = WaSafe::ingest(reader, *sink);

        // Поиск по полному пути работает на любом уровне вложенности, включая
        // элементы массива и члены структуры: "top.cpu.regs[3].valid".
        const auto sig = db.find(argv[1]);
        if (!sig) {
            std::println(stderr, "signal not found: {}", argv[1]);
            return 1;
        }

        // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
        const WaSafe::TimeRange range{std::atoll(argv[2]), std::atoll(argv[3])};

        std::println("changes of {} in [{}, {}):", sig->fullPath(), range.begin, range.end);

        // Ленивый курсор подгружает только пересекающиеся с диапазоном блоки.
        auto cursor = sig->changes(range);
        while (cursor.next()) {
            std::print("  @{:>10} = ", cursor->time);
            // Для композита value_at собрал бы агрегат; курсор отдаёт листовые виды.
            const auto& vv = cursor->value;
            if (vv.kind() == WaSafe::ValueKind::LOGIC)
                std::print("{}", vv.logic().toString());
            else if (vv.kind() == WaSafe::ValueKind::REAL)
                std::print("{}", vv.real());
            else if (vv.kind() == WaSafe::ValueKind::STRING)
                std::print("\"{}\"", vv.string());
            std::println("");
        }

        // Снимок составного значения целиком в момент range.begin.
        std::print("snapshot @{}: ", range.begin);
        printValue(sig->valueAt(range.begin));
        std::println("");
    }
    catch (const WaSafe::Exception& e) {
        std::println(stderr, "error: {}", e.what());
        return 1;
    }
    return 0;
}
