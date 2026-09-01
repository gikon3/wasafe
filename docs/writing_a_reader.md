# Как сделать проект-формат (Reader) поверх wasafe

Чтение конкретных форматов (VCD, FST, собственные бинарные дампы) в wasafe не
входит. Библиотека определяет только контракт, а парсер живёт в отдельном
проекте, зависящем от `wasafe`. Ядро о нём ничего не знает: реестра форматов и
автоопределения по сигнатуре нет — нужный `Reader` создаёт пользователь.

## Контракт

```cpp
#include "wasafe/io/reader.hpp"

class MyReader final : public WaSafe::Reader {
public:
    explicit MyReader(std::filesystem::path path);

    [[nodiscard]] std::string_view format() const override { return "myfmt"; }

    /// Полный разбор: заголовок + поток значений.
    void read(WaSafe::Builder& sink) override;

    /// Только заголовок (иерархия и типы). По умолчанию делегирует к read().
    void readHeader(WaSafe::Builder& sink) override;
};
```

`format()` нужен только для диагностики. Ошибки сообщаются броском
`WaSafe::Exception` (модель ошибок — исключения, не коды возврата).

## Протокол наполнения

`Builder` ([wasafe/io/builder.hpp](../include/wasafe/io/builder.hpp)) — единственный
приёмник; он же скрывает, как данные лягут в хранилище. Протокол двухфазный:

```cpp
void MyReader::read(WaSafe::Builder& sink) {
    using namespace WaSafe;

    // 1. Заголовок.
    sink.setTimeScale({.exponent = static_cast<int>(TimeUnit::NS), .scale = 1});
    sink.beginScope("top", ScopeKind::MODULE);
    const SignalId clk = sink.declareVar("clk", makeScalar());
    const SignalId bus = sink.declareVar("bus", makeVector(7, 0));
    sink.declareVar("clk_mirror", makeScalar(), clk);   // алиас: общий поток
    sink.endScope();
    sink.headerDone();

    // 2. Значения: монотонно неубывающее время + изменения потоков.
    sink.setTime(0);
    sink.valueChange(clk, /* ValueView */ ...);
    sink.setTime(10);
    sink.valueChange(bus, ...);
}
```

Правила, которые стоит помнить:

- композитный тип (packed-структура, массив) объявляется **одним** `declareVar`
  с композитным `Type`; разворачивать его в члены не нужно — это делает ядро;
- `declareVar` возвращает id **листового потока**; для unpacked-композита единого
  потока нет и id невалиден — значения пишутся в потоки его листьев;
- чтобы узнать id этих листьев, передайте параметр `leaves` — span размером
  `expansionStreamCount(type)`. Ядро заполнит невалидные элементы выделенными
  потоками, а валидные примет как есть: так лист привязывается к УЖЕ
  существующему потоку (алиас на уровне элемента — в VCD один код на несколько
  ссылок обычное дело). Порядок span — порядок разворачивания: глубина-первым,
  элементы массива по `ArrayType::ordinalOf` (`ord = 0` — это `indexLeft`, для
  `[3:0]` первым идёт `[3]`; для индекса вне диапазона массива метод вернёт
  `std::nullopt`), члены структуры — в порядке объявления:

  ```cpp
  Type const memT = makeArray(makeVector(7, 0), 0, 3);   // logic [7:0] mem [0:3]
  std::vector<SignalId> leaves(expansionStreamCount(memT));
  leaves[2] = alreadyKnownStream;                        // mem[2] — алиас
  sink.declareVar("mem", memT, std::nullopt, leaves);    // остальные выделит ядро
  ```

  Для типа, представимого одним потоком (лист или packed-композит),
  `expansionStreamCount` равен 0 и span обязан быть пустым: его поток задаётся
  параметром `alias`. Пустой span сохраняет прежнее поведение — id листьев тогда
  выводятся из порядка разворачивания, который остаётся определённым;
- `setTime` монотонно неубывающее; `finish()` вызывать не нужно — это делает
  `ingest()`.

## Первым делом: прогоните себя через валидирующий приёмник

Приёмники ради горячего пути контракт почти не проверяют — каждое нарушение стоило
бы проверки на каждое изменение, а их в дампе сотни миллионов. Поэтому ошибка в
парсере проявляется не там, где сделана, и не тем, чем обещано моделью ошибок:

| нарушение | что происходит без проверки |
| --- | --- |
| вид значения не тот, что у потока (`real` в logic-поток) | `std::bad_variant_access`, а не `WaSafe::Exception` |
| значение **шире** потока | молча обрезается |
| значение **уже** потока | **запись за границу массива**: биты за `width()` читаются как `X`, b-бит у `X` единичный, а план `bval` двухзначного блока не заведён |
| `valueChange` до первого `setTime` | изменение уезжает в момент 0 |
| время пошло назад | блоки и индекс собираются молча неверными |
| лишний `endScope` | иерархия перекашивается, сигналы попадают не в тот scope |
| `SignalId` не от этого приёмника | изменение молча теряется |

Всё это ловит декоратор `makeValidatingBuilder()`: он проверяет фазу протокола,
баланс scope'ов, монотонность времени, известность каждого `SignalId` (включая
`alias` и `leaves`), а также вид и ширину каждого значения — и бросает
`WaSafe::Exception` с указанием потока и обеих величин.

```cpp
#include "wasafe/io/builder.hpp"

MyReader reader{"dump.myfmt"};

auto sink  = WaSafe::makeIndexingBuilder("dump.wsfstore");  // или makeMemoryBuilder()
auto guard = WaSafe::makeValidatingBuilder(*sink);          // sink обязан пережить guard

WaSafe::Database db = WaSafe::ingest(reader, *guard);
```

Декоратор ничего не меняет в результате: корректный дамп через него даёт ту же БД,
что и напрямую. Держать его в готовом парсере не нужно — каждое `valueChange`
стоит поиска в хеш-таблице; это инструмент отладки и регрессионных тестов
формата.

## Значения: схлопывание 9-значной логики

Внутри wasafe логика **четырёхзначная** (`0`, `1`, `z`, `x` — см.
[logic.hpp](../include/wasafe/types/logic.hpp)): каждый бит кодируется парой
`(aval, bval)`, то есть два бита на бит. Источники же нередко отдают mvl9
(VHDL `std_logic`, FST): `U X 0 1 Z W L H -`. Схлопывание делает **парсер**, и
делать его нужно одинаково во всех форматах — иначе один дамп будет читаться
по-разному в зависимости от того, кто его открыл.

Канонический маппинг:

| mvl9 | → | почему |
| --- | --- | --- |
| `0`, `1`, `X`, `Z` | `0`, `1`, `x`, `z` | прямое соответствие |
| `L` | `0` | слабый ноль — это ноль |
| `H` | `1` | слабая единица — это единица |
| `W` | `x` | слабое неизвестное — всё равно неизвестное |
| `U` | `x` | неинициализированное значение неизвестно |
| `-` | `x` | don't care |

⚠️ **Ловушка**: `logicFromChar` (а значит и `LogicVector::assignFromChars`)
отправляет в `x` ВСЁ, кроме `0`, `1`, `z`, `Z`. Скормив ему `L` или `H`
напрямую, вы молча получите `x` вместо `0`/`1`. Нормализовать надо ДО:

```cpp
/// mvl9 → 4-значная логика. Вызывать перед assignFromChars.
constexpr char collapseMvl9(char c) noexcept {
    switch (c) {
        case 'L': case 'l': return '0';
        case 'H': case 'h': return '1';
        case 'W': case 'w':
        case 'U': case 'u':
        case '-':
            return 'x';
        default:
            return c;  // 0 1 x X z Z — как есть
    }
}
```

Расширять ядро до девяти значений сознательно не стали: это сломало бы компактную
бит-планную кодировку (2 бита на бит), а различия `L`/`H`/`W` за пределами
VHDL-моделирования почти никому не нужны.

## Подключение пользователем

```cpp
#include "wasafe/io/ingest.hpp"

MyReader reader{"dump.myfmt"};

auto sink = WaSafe::makeMemoryBuilder();                   // всё в ОЗУ
// либо: auto sink = WaSafe::makeIndexingBuilder("dump.wsfstore");  // ленивый путь

WaSafe::Database db = WaSafe::ingest(reader, *sink);
```

Режим хранения выбирает пользователь приёмником, а не парсер. Отдельного шага
регистрации нет, поэтому и линковать библиотеку формата целиком
(`$<LINK_LIBRARY:WHOLE_ARCHIVE,...>`, как требовалось при саморегистрации) больше
не нужно.

## Что стоит знать о стоимости ingestion

Парсер сам ничего не буферизует: он отдаёт изменения по одному, а память под них
расходует приёмник. Это важно для больших дампов:

- `makeMemoryBuilder()` держит всю диаграмму в ОЗУ — примерно 12 байт на изменение
  значения;
- `makeIndexingBuilder(store, opts)` пишет блоки на диск **потоково**, поэтому его
  пиковая память не зависит ни от размера дампа, ни от числа сигналов:

  ```cpp
  auto sink = WaSafe::makeIndexingBuilder(store, {
      .blockChanges = 4096,        // сколько изменений копить в блоке (0 — умолчание)
      .bufferBytes  = 64u << 20,   // общий потолок накопителей в ОЗУ
  });
  ```

  Блок уходит на диск по заполнении, а при превышении `bufferBytes` самые крупные
  накопители сбрасываются досрочно. Гигабайтные дампы разбираются на десятках
  мегабайт RSS.

От парсера тут требуется только одно: **не копить изменения у себя**. Читайте
источник потоково и сразу передавайте значения в `valueChange` — буфер фиксированного
размера вместо чтения файла целиком (и тем более вместо `mmap`, страницы которого
попадут в RSS процесса).

`ValueView`, передаваемый в `valueChange`, невладеющий: приёмник копирует данные
внутрь себя немедленно, поэтому парсер вправе переиспользовать один и тот же
буфер-скретч (`LogicVector`) на все изменения.

## Повторное открытие store

Файл, который пишет `makeIndexingBuilder()`, **самодостаточен**: вместе с блоками
значений в него уезжают иерархия, типы и геометрия блоков. Поэтому второй раз
парсер не нужен вовсе — и исходный дамп тоже:

```cpp
#include "wasafe/io/store.hpp"

WaSafe::Database db = WaSafe::openStore("dump.wsfstore");   // без Reader и без dump.myfmt
auto sig = db.find("top.cpu.pc");
```

Спутников у файла нет, копировать и переносить его можно как есть. Порядок байт
фиксированный (little-endian), так что файл читается и на машине другой
архитектуры.

Раскладка — заголовок, блоки, метаданные, футер:

| часть | когда пишется |
| --- | --- |
| заголовок (магия, версия) | в конструкторе builder'а |
| блоки значений | потоково, по мере разбора |
| метаданные (иерархия, типы, геометрия) | в `finish()` |
| футер (смещение и размер метаданных) | в `finish()`, в самом конце файла |

Точка входа при открытии — футер: он в конце, его положение известно всегда.
Отсюда одно следствие, о котором стоит знать: **прерванная ingestion оставляет
файл без хвоста**, и открыть его нельзя. Заголовок при этом на месте, поэтому
`openStore` отличает «недописанный store» от «это вообще не наш файл» и говорит
об этом разными сообщениями.

Путь «чтение на месте» из следующего раздела этим не пользуется: там store не
создаётся вовсе, а индекс строится из нативного индекса формата при каждом
открытии.

## Ленивый доступ прямо в исходный файл

Путь «Reader → Builder» нормализует источник в `*.wsfstore`: изменения,
перемешанные по времени, перегруппировываются по потокам. Для VCD это
неизбежно — чтобы отдать один сигнал за диапазон, иначе пришлось бы перечитать
в этом диапазоне всех. Но если формат **уже сгруппирован по сигналам и несёт
собственный блочный индекс** (FST), нормализация — лишний проход и удвоение
занятого диска: данные уже лежат так, как надо.

Такой формат может отдать `Database` с ленивым доступом прямо в свой файл,
минуя `Builder`. `LazyStorage` про файлы ничего не знает — он знает две
абстракции: [`SignalIndex`](../include/wasafe/storage/signal_index.hpp) («какие
блоки у потока и где они») и
[`BlockSource`](../include/wasafe/storage/block_source.hpp) («дай блок»).

### Свой BlockSource

Контракт короткий — один обязательный метод и один необязательный:

```cpp
class WaSafe::BlockSource {
public:
    virtual DecodedBlock decode(SignalId id, const BlockRef& ref) const = 0;
    // Необязательный: см. «Чтение из нескольких потоков» ниже.
    virtual std::unique_ptr<BlockSource> duplicate() const { return nullptr; }
};
```

Возвращать надо сразу `DecodedBlock`, а не байты: промежуточная сериализация в
наш формат блока не нужна вовсе. `BlockRef` описывает, ЧТО собирать:

| поле | кто заполняет | смысл |
| --- | --- | --- |
| `time` | вы | покрытие блока; по нему ядро ищет блок бинарным поиском |
| `offset` | вы | смещение в **вашем** файле — ядро его не разыменовывает |
| `cookie` | вы | 64 бита произвольного смысла; ядро НЕ интерпретирует |
| `storedSize`/`rawSize`/`codec` | — | нужны только источникам нашего формата |

```cpp
class MyBlockSource final : public WaSafe::BlockSource {
public:
    explicit MyBlockSource(const std::filesystem::path& path) : file_{path} {}

    [[nodiscard]] WaSafe::DecodedBlock decode(
            WaSafe::SignalId id, const WaSafe::BlockRef& ref) const override {
        const Chunk& chunk = chunkAt(ref.offset);   // см. про кэш чанка ниже
        WaSafe::DecodedBlock block{WaSafe::ValueKind::LOGIC, widthOf(id)};
        for (const auto& [time, bits] : chunk.chainOf(id, ref.cookie))
            block.append(time, WaSafe::ValueView{bits});
        return block;
    }
};
```

Блоки в НАШЕЙ сериализации (`*.wsfstore`, буфер в ОЗУ) обслуживает промежуточный
слой [`RawBlockSource`](../include/wasafe/storage/raw_block_source.hpp): он
реализует `decode` через `readBlock`, и наследнику остаётся только прочитать
байты по смещению. Формату со своим блочным устройством этот слой не нужен —
наследуйтесь прямо от `BlockSource`.

### Где держать контекст блока

Источник получает `SignalId` и `BlockRef` — больше ничего. Всё, что нужно сверх
этого, кладётся в одно из двух мест:

- **до 64 бит — в `cookie`**. Он едет внутри самого индекса и переживает его
  сериализацию в секцию метаданных store, так что побочная таблица не нужна.
  Типичное применение — упаковать пару `uint32` (позиция и длина цепочки
  изменений внутри чанка) или индекс в собственном массиве;
- **больше — в своих структурах**, ключуя тем же, чем ядро ключует кэш:
  `(SignalId, offset, cookie)`. Потолка по размеру там нет, но и в store это
  не попадёт — источник строит такую таблицу заново при открытии.

`SignalId` здесь — идентичность потока, а не индекс в каком-либо массиве. Она
нужна именно потому, что в чужом формате один чанк несёт изменения нескольких
потоков и `offset` у их блоков общий.

### Сборка Database

```cpp
MyReader reader{path};

// 1. Иерархию строит штатный Builder — разворачивание композитов достаётся даром.
WaSafe::Database hdr = WaSafe::ingestHeader(reader, *WaSafe::makeMemoryBuilder());

// 2. Индекс — из НАТИВНОГО индекса формата, за то же открытие.
//    offset ссылается в исходный файл, никакой store не создаётся.
WaSafe::SignalIndex idx = reader.buildIndex();

// 3. Ленивое хранилище прямо над исходным файлом.
WaSafe::Database db{hdr.hierarchy(),
        std::make_unique<WaSafe::LazyStorage>(std::move(idx), std::make_unique<MyBlockSource>(path))};
```

Иерархия передаётся копией: она строится один раз за открытие, и на фоне разбора
заголовка эта копия незаметна.

Взамен достаются готовыми LRU-кэш блоков, курсоры по диапазону,
`valueAt`/`nextChange`/`prevChange` и пакетный `openCursor(span)` — то есть весь
`LazyStorage` целиком.

### Подводный камень: чанк ≠ блок

В форматах вроде FST единица ввода-вывода — **чанк**, сжатый целиком, а единица
нашего индекса — **(поток, блок)**. Наивный `decode` распакует один и тот же
чанк заново для каждого сигнала, и LRU ядра не поможет: он кэширует
`DecodedBlock` по потокам, а не сырой чанк.

Значит источнику нужен собственный кэш последнего распакованного чанка. Он
эффективен ровно потому, что пакетный курсор идёт по времени и трогает все
потоки одного чанка подряд.

### Чтение из нескольких потоков

Один экземпляр `Database` рассчитан на ОДИН поток исполнения: `LazyStorage`
мутирует LRU-кэш прямо в `const`-методах, а `FileBlockSource` двигает позицию
своего `ifstream`. Мьютексов в библиотеке нет ни одного и не появится —
второму потоку полагается свой экземпляр, который делает `Database::duplicate()`:
неизменяемое (иерархия, индекс, значения) разделяется, изменяемое (кэш,
дескриптор файла) у дубликата своё.

```cpp
WaSafe::Database ui = openMyFormat(path);
WaSafe::Database background = ui.duplicate();   // по одной БД на поток
```

Ядро само сделать дубликат вашего источника не может — как переоткрыть файл,
знаете только вы. Поэтому `duplicate()`:

```cpp
[[nodiscard]] std::unique_ptr<WaSafe::BlockSource> duplicate() const override {
    return std::make_unique<MyBlockSource>(path_);   // новый дескриптор, НЕ копия позиции
}
```

Это не копия состояния: свои буферы и свой кэш чанка (см. предыдущий раздел)
дубликат начинает с нуля, а разделять с оригиналом стоит только то, что после
сборки не меняется, — например неизменяемую карту блоков через `shared_ptr`.

Метод необязателен. Если его не реализовать, формат остаётся рабочим, но
однопоточным: `Database::duplicate()` на такой БД бросит `Exception`.

Проверять реализацию удобнее всего санитайзером — в профилях conan для этого
есть готовый `clang-tsan`.

### Уровень ниже: свой Storage

Если геометрия формата вообще не ложится на «у каждого потока свой
упорядоченный список блоков», можно реализовать
[`Storage`](../include/wasafe/storage/storage.hpp) целиком: шесть чисто
виртуальных методов (`timeRange`, `timeScale`, `valueAt`, `openCursor(SignalId)`,
`nextChange`, `prevChange`), остальное имеет реализации по умолчанию. Свободы
больше, но курсоры и кэш придётся написать самому. Размножение по потокам тогда
тоже на вас — `Storage::duplicate()` с той же семантикой.

## Скелет проекта

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(wasafe_myfmt VERSION 0.1 LANGUAGES CXX)

find_package(wasafe REQUIRED)

add_library(wasafe_myfmt src/my_reader.cpp)
add_library(wasafe::myfmt ALIAS wasafe_myfmt)

target_include_directories(wasafe_myfmt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(wasafe_myfmt PUBLIC wasafe::wasafe)
```

`conanfile.py`:

```python
from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout


class WaSafeMyFmtConan(ConanFile):
    name = "wasafe_myfmt"
    version = "0.1"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("wasafe/0.1")

    def validate(self):
        check_min_cppstd(self, "23")   # C++23 приходит из профиля Conan

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()
```

Собирать с `-s compiler.cppstd=23`: профиль Conan по умолчанию даёт 20, а wasafe
требует минимум 23.

## Обратный путь: Writer

Экспорт устроен симметрично: [`Writer`](../include/wasafe/io/writer.hpp) читает
готовую БД через её публичный интерфейс и пишет файл. Реестра форматов тут тоже
нет — нужный `Writer` создаёт сам пользователь.

Мерджить курсоры листьев по времени вручную НЕ нужно: у ядра есть перечисление
листьев и курсор сразу по нескольким узлам.

```cpp
void MyWriter::write(const WaSafe::Database& db, const std::filesystem::path& path) {
    // Фаза объявлений: все листовые узлы дизайна, обходом в глубину.
    // Вложенные scope обходятся рекурсивно; packed-структура — ОДИН лист
    // (наружу она отдаётся одним потоком, члены суть его битовые срезы).
    const std::vector<WaSafe::NodeId> leaves = db.leafNodes(db.root().id());
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        const WaSafe::Signal sig = db.signalHandle(leaves[i]);
        declare(identOf(i), sig.width(), sig.fullPath());
    }

    // Фаза значений: ОДИН курсор по всем листьям сразу вместо N независимых.
    auto cur = db.changes(leaves, db.timeRange());
    while (cur.next())
        emit(identOf(cur->source), cur->time, cur->value);
}
```

Ключевое здесь — `cur->source`: номер источника, слившего своё изменение в общий
поток. Это **индекс в том самом векторе**, который был передан в `changes()`,
поэтому таблица идентификаторов индексируется напрямую, без поиска по `SignalId`.
Индекс, а не `SignalId`, потому что `SignalId` не различал бы запрошенное: алиасы
VCD дают несколько узлов на один поток, а packed-члены одного вектора — одну и ту
же проекцию.

Что гарантирует курсор:

- изменения идут в неубывающем порядке времени;
- при совпадении времени порядок выдачи следует порядку узлов в переданном
  векторе;
- `cur->value` — невладеющий вид, валидный до следующего `next()`; если значение
  нужно пережить шаг, его надо скопировать.

Рабочий скелет целиком — [examples/export_changes.cpp](../examples/export_changes.cpp).

Тот же мультикурсор обслуживает и второй сценарий — отрисовку окна в GUI:
`db.changes(visibleSignals, window)` вместо N независимых курсоров.

## Что можно взять за основу

Реализация VCD-парсера (`VcdReader`/`sniff`) и заготовка FST лежали в этом
репозитории до выноса форматов и остались в истории — коммит `955275e`:

```bash
git show 955275e:plugins/vcd/src/vcd_reader.cpp > src/vcd_reader.cpp
git show 955275e:plugins/vcd/include/wasafe/formats/vcd_reader.hpp > include/.../vcd_reader.hpp
```

При переносе убрать из них статическую саморегистрацию (`ReaderRegistration`) —
этого механизма больше нет.

Рабочий минимальный пример источника, следующего этому же контракту, —
[examples/demo_reader.hpp](../examples/demo_reader.hpp).
