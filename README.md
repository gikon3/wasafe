# wasafe — backend для хранения временных диаграмм

Библиотека на C++23 для хранения и доступа к временным диаграммам (waveforms)
из цифрового моделирования. Даёт **единый интерфейс к сигналам** независимо от
их типа и уровня вложенности, поддерживает **структуры и многомерные массивы
SystemVerilog**, **единый интерфейс записи из любого формата** и **ленивую
подгрузку по временным диапазонам и именам** без полной загрузки диаграммы
в память.

**Чтение конкретных форматов (VCD, FST, …) в этот проект не входит.** Библиотека
определяет контракт [`Reader`](include/wasafe/io/reader.hpp); парсеры живут в
отдельных проектах, а какой из них использовать — решает пользователь, передавая
его в [`ingest()`](include/wasafe/io/ingest.hpp). Ни реестра форматов, ни
автоопределения по сигнатуре в ядре нет — см. [docs/writing_a_reader.md](docs/writing_a_reader.md).

> Статус: **работает end-to-end**. Конвейер «Reader → нормализованный store →
> ленивый или in-memory доступ» собирается и проходит 51 тест (GoogleTest).
> Незакрытые места помечены `TODO(impl)` — их четыре, все локальные
> (см. [Что не доделано](#что-не-доделано)).

---

## Как закрыты требования

| Требование | Решение |
| --- | --- |
| Единый интерфейс к сигналам вне зависимости от типа и вложенности шины | [`Signal`](include/wasafe/model/signal.hpp) — один лёгкий хэндл для скаляра, шины, структуры, объединения и элемента массива. Те же методы `valueAt`/`changes`/`children`/`operator[]` на любом уровне. |
| Многомерные массивы и структуры из SV | Система типов [`type.hpp`](include/wasafe/types/type.hpp): `StructType`, `ArrayType` (многомерность — через вложение массивов), `EnumType`, `VectorType`. Packed-члены описываются битовыми срезами родительского потока (`BitSlice` в [hierarchy.hpp](include/wasafe/model/hierarchy.hpp)). |
| Общий интерфейс записи из любого формата | [`Builder`](include/wasafe/io/builder.hpp) — единый приёмник ingestion. Любой [`Reader`](include/wasafe/io/reader.hpp) (VCD/FST/…, реализуется вне этого проекта) пишет только через него и ничего не знает о хранилище; связка — [`ingest(reader, sink)`](include/wasafe/io/ingest.hpp). |
| Доступ по диапазонам времени или имени сигнала в файл, динамическая подгрузка без полной загрузки в память | [`Storage`](include/wasafe/storage/storage.hpp) + [`LazyStorage`](include/wasafe/storage/lazy_storage.hpp) + [`SignalIndex`](include/wasafe/storage/signal_index.hpp): в памяти только метаданные и индекс блоков; значения тянутся блоками по запросу `changes(range)` / `valueAt`. Поиск по имени — [`Database::find`](include/wasafe/storage/database.hpp). |
| C++23, CMake + Conan | `std::span`, `std::print`, ranges, `std::variant`. Стандарт задаётся профилем Conan (`-s compiler.cppstd=23`), а не `CMAKE_CXX_STANDARD`. Сборка через [`conanfile.py`](conanfile.py) + [`CMakeLists.txt`](CMakeLists.txt). |

Модель ошибок — **исключения**: всё, что может провалиться, бросает
[`WaSafe::Exception`](include/wasafe/core/exception.hpp) (наследник
`std::runtime_error`). Кодов возврата и `std::expected` в API нет; `std::optional`
используется там, где отсутствие результата — не ошибка (`find`/`findScope`).

---

## Архитектура

Слои зависят только «сверху вниз»; ядро не знает о конкретных форматах.

```plaintext
   внешние      ┌────────────────────────────────────────────────┐
   проекты      │ Reader (VCD, FST, …) — реализуют контракт ядра │
                └───────────────────────┬────────────────────────┘
                                        │ ingest(reader, sink)
                ┌───────────────────────▼──────────────────────┐
   io/          │ Builder (MemoryBuilder | IndexingBuilder)    │  единый ingestion
                └───────────────────────┬──────────────────────┘
                                        ▼
   storage/     ┌────────────────────────┐   ┌──────────────────┐
                │   Database             │──►│  Storage         │  значения
                │ (иерархия + значения)  │   │  ├ MemoryStorage │
                └────────────┬───────────┘   │  └ LazyStorage   │◄─ SignalIndex (блоки)
                             │               └──────────────────┘
   model/       ┌────────────┴───────────┐
                │ Hierarchy / Scope /    │  структура дизайна
                │ Signal (единый хэндл)  │
                └────────────┬───────────┘
   types/       ┌────────────┴───────────┐
                │ TypeDescriptor, Value, │  типы SV и значения (4-зн. логика)
                │ Logic                  │
   core/        └─ time / error / ids ───┘  базовые типы
```

### Единый интерфейс сигнала

`Signal` одинаков для всего. Композит (struct/array) раскрывается через те же
`children()` / `child(name)` / `operator[]`, а доступ к значению (`valueAt`,
`changes`) единообразен — для композита значение собирается из листьев.

```cpp
WaSafe::Signal s = *db.find("top.cpu.regs[3].ctrl");   // любой уровень вложенности

s.type();                       // дескриптор SV-типа
s.valueAt(1500);               // значение в момент времени (агрегат для композита)
for (auto field : s.children()) // члены структуры / элементы массива — те же Signal
    use(field.name(), field.valueAt(1500));

auto cur = s.changes({1000, 2000});  // ленивый курсор по диапазону
while (cur.next())
    use(cur->time, cur->value);
```

### Несколько сигналов за один проход

Отрисовать окно с полусотней сигналов или выгрузить дизайн во внешний формат —
это не N независимых курсоров, а один:

```cpp
auto cur = db.changes(nodes, {1000, 2000});   // nodes — вектор NodeId
while (cur.next())
    draw(nodes[cur->source], cur->time, cur->value);
```

Курсор сливает потоки по времени и в `cur->source` сообщает, чьё изменение
отдал, — это индекс в переданном векторе (индекс, а не `SignalId`: алиасы VCD
дают несколько узлов на один поток). При совпадении времени порядок выдачи
следует порядку узлов в запросе.

Вход для экспортёра — `db.leafNodes(db.root().id())`: все листья дизайна обходом
в глубину. Мерджить курсоры листьев вручную не нужно, см.
[examples/export_changes.cpp](examples/export_changes.cpp).

### Ленивая подгрузка

Ленивый режим включается выбором приёмника: `makeIndexingBuilder(store)` пишет
нормализованные блоки (`*.wsfstore`) и индекс, а полученная БД читает их по
запросу. `changes(range)` открывает курсор, который тянет только пересекающиеся
с `range` блоки (`SignalLocator::blocksIn`); `LazyStorage` держит ограниченный
LRU-кэш распакованных блоков (`LazyStorageOptions::cacheBytes`), поддерживает
`prefetch`/`release`.

Для форматов без встроенной индексации (VCD) индекс строится за один проход и
сохраняется в сайдкар `*.wsfidx`.

**Запись тоже ленивая.** `IndexingBuilder` не собирает диаграмму в ОЗУ: блок
уходит на диск, как только накопил `blockChanges` изменений, а сверх того
действует общий потолок буфера — при его превышении самые крупные накопители
сбрасываются досрочно. Поэтому пиковая память не зависит ни от размера дампа,
ни от числа сигналов, и ограничения на размер обрабатываемого файла нет:

```cpp
auto sink = WaSafe::makeIndexingBuilder(store, {
    .blockChanges = 4096,        // 0 — значение по умолчанию
    .bufferBytes  = 64u << 20,   // потолок накопителей в ОЗУ
});
```

Раскладка блока уплотнена: бит-планы `aval`/`bval` хранятся раздельно, и `bval`
не заводится вовсе, пока в блоке не встретилось `x`/`z` (двухзначные блоки —
подавляющее большинство); метки времени лежат как база блока плюс 32-битные
смещения ([`TimeColumn`](include/wasafe/types/time_column.hpp)) с однократным
повышением до 64-битных, если разность перестала помещаться. На изменение
уходит 12 байт вместо 24. На диске метки дополнительно пишутся LEB128-дельтами.

> Ограничение: сайдкар пока не хранит иерархию, поэтому `*.wsfstore` читается
> только в рамках процесса, который его построил — `Database::open(path)` нет.
> Повторное открытие дампа требует повторного разбора источника.

### Единый интерфейс записи (ingestion)

Вариант 1 — источником служит парсер формата (из отдельного проекта):

```cpp
Vcd::VcdReader reader{"dump.vcd"};                       // внешний проект-формат
auto sink = WaSafe::makeMemoryBuilder();                 // или makeIndexingBuilder(store)
WaSafe::Database db = WaSafe::ingest(reader, *sink);
```

Вариант 2 — наполнение из своего кода тем же приёмником:

```cpp
auto b = WaSafe::makeMemoryBuilder();
b->setTimeScale({.exponent = -12, .scale = 1});
b->beginScope("top", ScopeKind::MODULE);
auto clk = b->declareVar("clk", makeScalar());
b->declareVar("req", makeStruct({...}, /*packed*/true));  // композит — одним вызовом
b->headerDone();
b->setTime(0);  b->valueChange(clk, ...);
b->finish();
auto db = b->takeDatabase();
```

Любой парсер наполняет хранилище только через `Builder`, поэтому поддержка нового
формата = новый `Reader` в своём проекте, без единой правки в ядре.

---

## Структура каталогов

```plaintext
include/wasafe/        публичные заголовки (API)
  core/                время, исключения, строгие id, string_map
  types/               логика 4-знач., система типов SV, представление значений,
                       поколоночные срезы (ColumnView, TimeColumn)
  model/               иерархия, Scope, единый Signal
  storage/             storage, индекс блоков, ленивый/in-memory, БД, запросы
  io/                  Builder, контракт Reader/Writer, ingest()
src/                   реализация ядра
examples/              dump_hierarchy, query_range, build_inmemory (+ DemoReader)
tests/                 модульные тесты (GoogleTest)
```

Парсеров форматов в дереве нет: они выносятся в отдельные проекты, зависящие от
этой библиотеки ([docs/writing_a_reader.md](docs/writing_a_reader.md)).

---

## Сборка

Требуется CMake ≥ 3.24, компилятор с C++23 (GCC 14+, Clang 18+, MSVC 19.40+) и Conan 2.

```bash
# зависимости + тулчейн
conan install . --output-folder=build --build=missing \
    -s compiler.cppstd=23 -s build_type=Release

# конфигурация и сборка
cmake --preset conan-release        # пресет создаёт conan
cmake --build --preset conan-release

# тесты
ctest --preset conan-release
```

Опции CMake/Conan: `WASAFE_WITH_ZSTD`, `WASAFE_BUILD_EXAMPLES`, `BUILD_TESTING`.

Зависимости (через Conan): `zstd` (сжатие блоков ленивого storage), `gtest` (тесты).

---

## Производительность

Замер на синтетическом VCD 2 ГиБ (8 193 сигнала, 77,1 млн изменений), внешний
парсер VCD, i7-12700K, NVMe. Эталон для сравнения — `vcd2fst` из GTKWave 3.3.128,
который решает ту же задачу «дамп → сжатый блочный формат с индексом»:

| | время | пик RSS | артефакт |
| --- | ---: | ---: | ---: |
| wasafe, `makeIndexingBuilder` | 24,3 с | 121 МиБ | 371 МБ (`*.wsfstore`) |
| gtkwave `vcd2fst` | 17,1 с | 152 МиБ | 367 МБ (`*.fst`) |

Пиковая память не растёт с размером входа (55 / 110 / 121 МиБ на дампах
100 МиБ / 500 МиБ / 2 ГиБ) — это следствие потоковой записи и потолка буфера.
Точечный `valueAt` по случайному сигналу и случайному моменту времени на
ленивой БД — около 13 мкс.

---

## Что не доделано

Четыре `TODO(impl)`, все локальные:

- [`decompress.cpp`](src/storage/decompress.cpp) — кодеки lz4/zlib (zstd работает;
  остальные нужны форматам вроде FST);
- [`type.cpp`](src/types/type.cpp) — `toSvString()` для полной SV-нотации типа;
- [`time.cpp`](src/core/time.cpp) — нормализация произвольного `exponent` к
  ближайшей приставке в `formatTimeScale()` и полный разбор в `parseTimeScale()`.

Крупнее, за рамками `TODO(impl)`:

- **[`SignalQuery`](include/wasafe/storage/signal_query.hpp) объявлен, но не
  реализован** — заголовок есть, парного `.cpp` нет, символов в библиотеке тоже.
  Пакетная выборка (`match`/`leavesUnder`/`where`/`prefetch`) даст ошибку
  компоновки; точечный `Database::find()` работает;
- **сайдкар не хранит иерархию**, поэтому `Database::open(path)` отсутствует и
  готовый store нельзя переоткрыть без повторного разбора источника;
- **значения не пакуются по битам**: 1-битный сигнал занимает в ОЗУ целое слово.
  Упаковка несовместима с текущим зеро-копи `LogicVectorView` (у изменения не
  будет непрерывной пары планов по вычислимому адресу) и требует отдельного
  захода — например, через битовое смещение во вьюхе.

Парсеры форматов и запись в форматы — задача отдельных проектов
([docs/writing_a_reader.md](docs/writing_a_reader.md)).
