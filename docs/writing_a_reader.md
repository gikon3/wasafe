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
  `[3:0]` первым идёт `[3]`), члены структуры — в порядке объявления:

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
