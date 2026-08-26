#pragma once

#include <filesystem>

#include "wasafe/export.hpp"
#include "wasafe/storage/database.hpp"

namespace WaSafe {

/// Открыть ранее записанный *.wsfstore.
///
/// Файл самодостаточен: иерархия, типы и геометрия блоков лежат в его хвосте,
/// поэтому ни парсер, ни исходный дамп не нужны. Значения подгружаются лениво,
/// как и при первом разборе через makeIndexingBuilder().
///
///   WaSafe::Database db = WaSafe::openStore("dump.wsfstore");
///   auto sig = db.find("top.cpu.pc");
///
/// Бросает Exception, если файл не открывается, не является store, записан
/// несовместимой версией или не дописан до конца (оборванная ingestion).
[[nodiscard]] WASAFE_API Database openStore(const std::filesystem::path& path);

}  // namespace WaSafe
