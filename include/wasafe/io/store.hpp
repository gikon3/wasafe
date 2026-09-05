#pragma once

#include <filesystem>

#include "wasafe/export.hpp"
#include "wasafe/model/database.hpp"
#include "wasafe/storage/lazy_storage.hpp"

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
/// opts задаёт потолок кэша распакованных блоков.
///
/// verifyBlocks включает сверку каждого прочитанного блока с контрольной суммой
/// из индекса. По умолчанию выключена: сумма считается по всем байтам блока и
/// потому заметна рядом с распаковкой. Метаданные сверяются ВСЕГДА, независимо
/// от флага: они читаются один раз на открытие.
///
/// Бросает Exception, если файл не открывается, не является store, записан
/// несовместимой версией, повреждён (не сошлась сумма метаданных) или не дописан
/// до конца (оборванная ingestion).
[[nodiscard]] WASAFE_API Database openStore(const std::filesystem::path& path, LazyStorageOptions opts = {},
        bool verifyBlocks = false);

}  // namespace WaSafe
