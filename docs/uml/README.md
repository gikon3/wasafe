# UML-диаграммы wasafe

Диаграммы написаны в формате **PlantUML** (`.puml`).

## Как отрендерить

**VS Code**: установить расширение `jebbs.plantuml`, нажать `Alt+D` для предпросмотра.

**Онлайн**: скопировать содержимое файла на [plantuml.com/plantuml/uml](https://www.plantuml.com/plantuml/uml).

**CLI**:

```bash
java -jar plantuml.jar docs/uml/*.puml   # → PNG рядом с файлом
```

---

## Диаграммы

| Файл | Что показывает |
| --- | --- |
| [01_architecture.puml](01_architecture.puml) | Слои библиотеки и зависимости между ними |
| [02_types.puml](02_types.puml) | Иерархия типов (TypeDescriptor) и система значений (Value, LogicVector) |
| [03_model.puml](03_model.puml) | Иерархия дизайна: ScopeNode/SignalNode, хэндлы Scope/Signal |
| [04_storage.puml](04_storage.puml) | Хранилище: Storage, MemoryStorage, LazyStorage, SignalIndex |
| [05_io.puml](05_io.puml) | I/O: Builder, контракт Reader/Writer, связка ingest(), внешние проекты-форматы |
| [06_sequence_open.puml](06_sequence_open.puml) | Последовательность: ingest источника + запрос по диапазону + valueAt |
| [07_signal_uniform.puml](07_signal_uniform.puml) | Демонстрация единого интерфейса Signal для всех типов |

## Рекомендуемый порядок чтения

1. **01** — общая картина, с чего начать
2. **06** — последовательная диаграмма даёт живой сценарий
3. **02** → **03** → **04** → **05** — детали каждого слоя
4. **07** — убедиться в единстве интерфейса Signal
