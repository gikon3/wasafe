#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string_view>

#include "core/byte_io.hpp"
#include "wasafe/core/exception.hpp"

/// Раскладка файла *.wsfstore. Он самодостаточен: значения, иерархия и
/// геометрия блоков лежат в одном файле, спутников у него нет.
///
///   заголовок  magic + версия            — пишется в конструкторе builder'а
///   блоки      потоковая запись          — смещения начинаются сразу за заголовком
///   метаданные секции иерархии и индекса — дописываются в finish()
///   футер      экстент, сумма, магия     — фиксированный размер в самом конце
///
/// Точка входа при открытии — футер: он в конце, его положение известно всегда.
/// Оборванная запись оставляет файл без футера, и это отличимо от «файл вообще
/// не наш» по заголовку, а от тихо испорченного — по контрольной сумме.
///
/// ВЕРСИИ. Номер версии — пара major.minor, упакованная в 32 бита (major в
/// старшей половине). Своя пара есть у файла и у каждой секции метаданных:
/// секции живут своей жизнью, и рассинхрон между ними должен быть диагностируем,
/// а не проявляться как «truncated» в случайном месте разбора.
///
///   - несовпадение MAJOR — отказ, безусловно и для файла, и для каждой секции;
///   - MINOR больше нашего читается: секция несёт свою длину, поэтому дописанный
///     в новой минорной версии хвост пропускается, а известное начало разбирается
///     как обычно. Отсюда правило для будущих правок формата: расширять секцию
///     можно только В КОНЕЦ и только с приращением minor.
///
/// Магия 'WSF1' — буквенный тег файла, а не версия: цифра в ней историческая,
/// версию несёт отдельное поле.
///
/// ОХВАТ КОНТРОЛЬНОЙ СУММЫ. CRC32 в футере покрывает секцию метаданных целиком
/// (иерархию и индекс). Значения (блоки) ею не покрыты: у блока своя сумма в
/// BlockRef::crc32, и проверяется она только при явно включённой проверке.
namespace WaSafe::StoreLayout {

/// Собрать номер версии из пары major.minor.
[[nodiscard]] constexpr std::uint32_t makeVersion(std::uint16_t major, std::uint16_t minor) noexcept {
    return (static_cast<std::uint32_t>(major) << 16u) | minor;
}

[[nodiscard]] constexpr std::uint16_t majorOf(std::uint32_t version) noexcept {
    return static_cast<std::uint16_t>(version >> 16u);
}

[[nodiscard]] constexpr std::uint16_t minorOf(std::uint32_t version) noexcept {
    return static_cast<std::uint16_t>(version & 0xFFFFu);
}

inline constexpr std::uint32_t kMagic = 0x3146'5357u;  // 'WSF1'
inline constexpr std::uint16_t kVersionMajor = 1;
inline constexpr std::uint16_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersion = makeVersion(kVersionMajor, kVersionMinor);
inline constexpr std::size_t kHeaderSize = 8;  // magic + version

inline constexpr std::uint32_t kFooterMagic = 0x5446'5357u;  // 'WSFT'
inline constexpr std::size_t kFooterSize = 24;               // metaOffset + metaSize + metaCrc32 + magic

/// Секции метаданных: заголовок (магия, версия, длина тела) и само тело.
inline constexpr std::uint32_t kHierarchyMagic = 0x4846'5357u;  // 'WSFH'
inline constexpr std::uint32_t kIndexMagic = 0x4946'5357u;      // 'WSFI'
inline constexpr std::uint32_t kHierarchyVersion = makeVersion(1, 0);
inline constexpr std::uint32_t kIndexVersion = makeVersion(1, 0);
inline constexpr std::size_t kSectionHeaderSize = 16;  // magic + version + payloadSize

/// Записать секцию целиком: заголовок и следом тело.
inline void writeSection(ByteWriter& w, std::uint32_t magic, std::uint32_t version,
        std::span<const std::byte> payload) {
    w.u32(magic);
    w.u32(version);
    w.u64(static_cast<std::uint64_t>(payload.size()));
    w.bytes(payload);
}

/// Разобрать заголовок очередной секции и вернуть вид на её тело; pos двигается
/// на конец секции — то есть за неизвестный хвост, если он есть.
///
/// what — имя секции для текста ошибки («hierarchy», «index»).
[[nodiscard]] inline std::span<const std::byte> readSection(std::span<const std::byte> meta, std::size_t& pos,
        std::uint32_t magic, std::uint32_t version, std::string_view what) {
    if (pos > meta.size() || meta.size() - pos < kSectionHeaderSize)
        throw Exception{std::format("store: truncated {} section header", what)};

    ByteReader r{meta.subspan(pos, kSectionHeaderSize)};
    std::uint32_t gotMagic = 0;
    std::uint32_t gotVersion = 0;
    std::uint64_t payloadSize = 0;
    if (!r.u32(gotMagic) || !r.u32(gotVersion) || !r.u64(payloadSize))
        throw Exception{std::format("store: truncated {} section header", what)};
    if (gotMagic != magic)
        throw Exception{std::format("store: bad {} section magic", what)};
    if (majorOf(gotVersion) != majorOf(version)) {
        throw Exception{std::format("store: unsupported {} section version {}.{}, expected {}.x", what,
                majorOf(gotVersion), minorOf(gotVersion), majorOf(version))};
    }

    const std::size_t body = pos + kSectionHeaderSize;
    if (payloadSize > meta.size() - body)
        throw Exception{std::format("store: {} section extends past metadata", what)};

    pos = body + static_cast<std::size_t>(payloadSize);
    return meta.subspan(body, static_cast<std::size_t>(payloadSize));
}

}  // namespace WaSafe::StoreLayout
