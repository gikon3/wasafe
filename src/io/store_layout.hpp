#pragma once

#include <cstddef>
#include <cstdint>

/// Раскладка файла *.wsfstore. Он самодостаточен: значения, иерархия и
/// геометрия блоков лежат в одном файле, спутников у него нет.
///
///   заголовок  magic + версия          — пишется в конструкторе builder'а
///   блоки      потоковая запись        — смещения начинаются сразу за заголовком
///   метаданные иерархия + индекс       — дописываются в finish()
///   футер      смещение и размер       — фиксированный размер в самом конце
///
/// Точка входа при открытии — футер: он в конце, его положение известно всегда.
/// Оборванная запись оставляет файл без футера, и это отличимо от «файл вообще
/// не наш» по заголовку.
namespace WaSafe::StoreLayout {

inline constexpr std::uint32_t kMagic = 0x3146'5357u;  // 'WSF1'
inline constexpr std::uint32_t kVersion = 1u;
inline constexpr std::size_t kHeaderSize = 8;  // magic + version

inline constexpr std::uint32_t kFooterMagic = 0x5446'5357u;  // 'WSFT'
inline constexpr std::size_t kFooterSize = 20;               // metaOffset + metaSize + magic

}  // namespace WaSafe::StoreLayout
