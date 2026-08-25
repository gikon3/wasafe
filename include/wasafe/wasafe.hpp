#pragma once

/// @file wasafe.hpp
/// Единый заголовок публичного API библиотеки временных диаграмм.
///
/// Слои:
///   core/    — базовые типы: время, ошибки, идентификаторы.
///   types/   — система типов SystemVerilog и представление значений.
///   model/   — иерархия дизайна и единый хэндл сигнала (Signal/Scope).
///   storage/ — хранилище значений: storage, индекс, ленивая подгрузка, БД.
///   io/      — единый интерфейс ingestion (Builder), контракт Reader/Writer для
///              внешних проектов-форматов и связка ingest().

#include "wasafe/config.hpp"
#include "wasafe/core/exception.hpp"
#include "wasafe/core/ids.hpp"
#include "wasafe/core/time.hpp"
#include "wasafe/io/builder.hpp"
#include "wasafe/io/ingest.hpp"
#include "wasafe/io/reader.hpp"
#include "wasafe/io/writer.hpp"
#include "wasafe/model/hierarchy.hpp"
#include "wasafe/model/scope.hpp"
#include "wasafe/model/signal.hpp"
#include "wasafe/storage/block_source.hpp"
#include "wasafe/storage/database.hpp"
#include "wasafe/storage/decoded_block.hpp"
#include "wasafe/storage/lazy_storage.hpp"
#include "wasafe/storage/memory_storage.hpp"
#include "wasafe/storage/raw_block_source.hpp"
#include "wasafe/storage/signal_index.hpp"
#include "wasafe/storage/signal_query.hpp"
#include "wasafe/storage/storage.hpp"
#include "wasafe/storage/value_cursor.hpp"
#include "wasafe/types/logic.hpp"
#include "wasafe/types/type.hpp"
#include "wasafe/types/value.hpp"
#include "wasafe/version.hpp"
