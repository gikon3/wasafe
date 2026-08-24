// Пример: программное наполнение хранилища через ЕДИНЫЙ интерфейс ingestion
// (Builder) и последующий единообразный доступ к структуре и массиву.
//
// Демонстрирует, что любой источник (здесь — код, а не парсер файла) пишет
// данные одинаково, а композитные SV-типы (struct, многомерный массив) затем
// читаются тем же Signal-интерфейсом, что и простые скаляры.

#include <print>

#include "wasafe/wasafe.hpp"

using namespace WaSafe;

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    auto builder = makeMemoryBuilder();

    builder->setTimeScale({.exponent = static_cast<int>(TimeUnit::PS), .scale = 1});

    // Иерархия: top { struct packed { logic[7:0] addr; logic valid; } req;
    //                 logic[3:0] mem [0:1][0:1]; }
    builder->beginScope("top", ScopeKind::MODULE);

    // Упакованная структура объявляется ОДНИМ declare_var с композитным типом.
    Type const reqT = makeStruct(
            {
                    StructMember{"addr", makeVector(7, 0), /*bit_offset*/ 1},
                    StructMember{"valid", makeScalar(), /*bit_offset*/ 0},
            },
            /*packed=*/true);
    const SignalId reqStream = builder->declareVar("req", reqT);  // один поток на packed-struct

    // Двумерный массив: элемент — вектор [3:0], два измерения [0:1][0:1].
    // Распакованный — единого потока нет (значения хранят листья-элементы).
    Type const memT = makeArray(makeArray(makeVector(3, 0), 0, 1), 0, 1);
    const SignalId memStream = builder->declareVar("mem", memT);
    (void)reqStream;
    (void)memStream;

    builder->endScope();
    builder->headerDone();

    // Поток значений (упрощённо).
    builder->setTime(0);
    builder->finish();

    try {
        auto db = builder->takeDatabase();

        // Единый обход: к структуре и к массиву применяется один и тот же API.
        if (auto req = db.find("top.req")) {
            std::println("req has {} members:", req->childCount());
            for (const Signal m : req->children()) {
                std::println("  .{} : {}", m.name(), toSvString(m.type()));
            }
        }
        if (auto mem = db.find("top.mem")) {
            // mem[1][0] — та же индексация на любом уровне вложенности массива.
            const Signal cell = (*mem)[1][0];
            std::println("mem[1][0] : {}", toSvString(cell.type()));
        }
    }
    catch (const Exception& e) {
        std::println(stderr, "build failed: {}", e.what());
        return 1;
    }
    return 0;
}
