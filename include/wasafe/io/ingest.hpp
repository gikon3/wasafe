#pragma once

#include "wasafe/export.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/io/reader.hpp"
#include "wasafe/model/database.hpp"

namespace WaSafe {

/// Связка «парсер формата → приёмник»: единственное место, где Reader встречается
/// с Builder. Реестра форматов у библиотеки нет — нужный Reader создаёт САМ
/// пользователь (реализации форматов живут в отдельных проектах), а режим
/// хранения задаётся выбранным builder'ом:
///
///   Vcd::VcdReader reader{"dump.vcd"};                  // внешний проект-формат
///   auto sink = makeMemoryBuilder();                    // либо makeIndexingBuilder(...)
///   Database db = ingest(reader, *sink);
///
/// Полный разбор источника: заголовок + значения, затем finish() и готовая БД.
[[nodiscard]] WASAFE_API Database ingest(Reader& reader, Builder& sink);

/// Разбор только заголовка: иерархия и типы без потока значений. Полезно, когда
/// нужен лишь обзор дизайна (дерево сигналов) без стоимости чтения изменений.
[[nodiscard]] WASAFE_API Database ingestHeader(Reader& reader, Builder& sink);

}  // namespace WaSafe
