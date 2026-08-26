#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include "io/base_builder.hpp"
#include "wasafe/storage/decoded_block.hpp"
#include "wasafe/storage/signal_index.hpp"

namespace WaSafe {

/// Builder, пишущий нормализованное блочное хранилище на диск (для ленивого
/// чтения). Запись ПОТОКОВАЯ: каждый поток накапливает не более одного блока,
/// который сжимается и уходит в store, как только заполнится (или раньше —
/// если суммарный объём накопителей превысил bufferBytes). Диаграмма целиком
/// в ОЗУ не собирается, поэтому пик памяти не зависит ни от размера дампа, ни
/// от числа сигналов.
///
/// Файл получается самодостаточным: заголовок в конструкторе, блоки по мере
/// разбора, а в finish() — иерархия, индекс и футер (см. store_layout.hpp).
class IndexingBuilder final : public BaseBuilder {
public:
    IndexingBuilder(std::filesystem::path store, IndexingOptions opts);
    void valueChange(SignalId id, ValueView value) override;
    void finish() override;
    [[nodiscard]] Database takeDatabase() override;

protected:
    SignalId allocStream(ValueKind kind, std::uint32_t width) override;

private:
    /// Накопитель одного потока. Вид и ширина продублированы рядом с блоком:
    /// DecodedBlock::kind()/width() приватны, а после сброса блок нужно
    /// пересоздать (именно пересоздать, а не очистить с сохранением ёмкости —
    /// удержанная по всем потокам ёмкость сама по себе нарушила бы бюджет).
    struct Stream {
        DecodedBlock block;
        ValueKind kind = ValueKind::NONE;
        std::uint32_t width = 0;
    };

private:
    /// Закодировать, сжать и записать накопленный блок потока; обновить индекс
    /// и освободить память накопителя. Для пустого потока — ничего не делает.
    void flushStream(std::uint32_t sid);

    /// Сбросить накопители, пока занятая ими память не уйдёт под половину
    /// потолка. Проходы идут с убывающим порогом по числу изменений, чтобы
    /// не плодить крошечные блоки у редко меняющихся сигналов.
    void relieve();

    /// Записать байты в store и подвинуть offset_. Единственная точка записи,
    /// поэтому смещение блока и фактический конец файла не расходятся.
    void writeRaw(std::span<const std::byte> data);

    /// Дописать хвост файла: метаданные (иерархия + индекс) и футер.
    void writeTrailer();

private:
    static constexpr std::size_t kDefaultBlockChanges = 4096;
    static constexpr std::size_t kDefaultBufferBytes = 64u << 20;

private:
    std::filesystem::path store_;
    std::size_t blockChanges_;
    std::size_t bufferBytes_;

    std::ofstream out_;
    std::uint64_t offset_ = 0;  ///< текущий конец store — смещение следующего блока

    std::vector<Stream> streams_;
    std::size_t buffered_ = 0;  ///< суммарный объём накопителей, байт

    // Время монотонно, поэтому глобальные min/max — первое и последнее изменение.
    TimeStamp firstTime_ = kNoTime;
    TimeStamp lastTime_ = kNoTime;

    SignalIndex index_;
    bool finished_ = false;
};

}  // namespace WaSafe
