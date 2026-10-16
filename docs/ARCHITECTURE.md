# Архитектура и контракты

## Слои

- `src/FrameScope.Core` — чистые модели профилей/настроек и сериализация.
- WPF-приложение в `src/` — редактор, persistence, опрос Windows и запуск Demo.
- `src/FrameScope.WindowsSensors` — read-only платформенный WDDM provider;
  он не добавляет Win32-зависимости в чистый Core.
- `native/` — общий HudCore, отдельные DX11/DX12/Vulkan backends, Launcher и Demo.
- `scripts/` — сборка, behavioral policy tests, проверка PE-зависимостей и упаковка.

```text
WPF editor -> profile/settings -> control snapshot -> shared mapping
                                                    |
                     own Demo -> API backend -> HudCore -> HUD
                                          |
                             Present intervals -> frame snapshot -> WPF
```

HudCore отвечает за общую компоновку и статистику. Backend отвечает за ресурсы
своего API, порядок кадров и безопасное завершение. Опрос датчиков не должен
блокировать Present. Demo — реальный renderer; его числа не подменяются
таймером редактора.

## Native ABI, версия 1

Источники истины: `native/include/framescope/protocol.hpp` и соответствующий
`NativeProtocol.cs`. Mapping называется `Local\FrameScope-<session>`, где session
— ровно **32 шестнадцатеричных символа**. Размер — **4096 байт**, packing — 8.

| Область | Смещение | Размер |
| --- | ---: | ---: |
| Control sequence | 24 | 8 байт |
| ControlData | 32 | 128 байт |
| Frame sequence | 256 | 8 байт |
| FrameData | 264 | 560 байт |
| Optional Intel sequence | 1016 | 8 байт |
| Optional IntelData | 1024 | 128 байт |

Magic — `0x46534350`, protocol version — 1. В `ControlData` первый `double`
находится на смещении 40, `updated_qpc` — 120. В `FrameData` массив графика
начинается на 80 и содержит до 120 `float`. Цвета — `0xAARRGGBB`.

У каждой области один writer. Seqlock reader принимает снимок только при
совпадающих чётных значениях sequence до/после копирования. Нельзя бесконечно
ждать writer на render thread. Неверные magic/version/размер/сессия означают
отказ соединения, а не попытку интерпретировать произвольную память. Presence
flags определяют доступность метрик независимо от числового значения.

Необязательный IntelControl/IntelData block имеет magic `0x494E544C`, version **1**,
64-bit availability mask и updated QPC; старый участник v1 может его игнорировать.
В payload offsets: magic 0, version 4, mask 8, updated QPC 16; девять `double`
на 24..88 — GPU/memory MHz, GPU/board W, fan RPM, voltage V, memory read/write
GB/s, memory °C; throttle flags — `uint64` на 96, далее 24 reserved bytes.
Основные control **128** и frame **560** байт не меняются. Строк причин N/A в ABI
нет: availability передаётся битами, детали относятся к диагностике provider.
Наличие блока не подтверждает доступность Level Zero/датчиков и не означает IGCL
integration. Истёкшие control/sensor snapshots
отвергаются; актуальный native reader использует предел 5 секунд.

При изменении layout синхронно меняются обе стороны ABI и проверки смещений.
Семантически несовместимый протокол требует нового номера версии.

## Запуск и поставка

Корень пакета содержит `FrameScope.exe`, дочерний каталог `native/`:

```text
FrameScope.Launcher.exe  FrameScope.Hud.dll
FrameScope.Vulkan.dll    FrameScope.Vulkan.json  FrameScope.Demo.exe
```

Launcher принимает `--demo PATH --api dx11|dx12|vulkan --session <32hex>
--mode windowed|borderless|exclusive [--duration seconds]`. В preview DX HUD
загружается стандартным LoadLibrary только собственным Demo. Vulkan explicit
layer включается окружением дочернего процесса, не через HKLM/HKCU registry.
Пути и аргументы не конкатенируются в shell-команду.

Отсутствующий helper или runtime останавливает запуск с объяснимой ошибкой;
нельзя молча запускать другое приложение/API. Native DLL и JSON manifest должны
оставаться из одной сборки. Динамические runtime DLL поставляются по PE-import
closure; драйверы и системные DLL не копируются.

Когда соответствующий target готов, рядом может поставляться собственный
`FrameScope.Intel.exe`: read-only helper для advanced Intel Level Zero Sysman.
Packaged capability описывает наличие файла, не успешный hardware probe.
Helper не настраивает частоты, power limits или вентиляторы; отсутствующий
installed `ze_loader.dll`/domain даёт N/A с причиной, а не установку драйвера.
Loader из System32 не распространяется вместе с приложением.

## Сохранение и завершение

Профиль хранит настройки, не активную mapping/session. `portable.txt` выбирает
переносимый storage рядом с приложением; иначе используется per-user storage.
Ошибку записи нельзя показывать как успешное сохранение. Повреждённые настройки
не должны приводить к запуску произвольного executable или потере исходного файла.

Одна сессия относится к выбранному Demo/swapchain. Закрытие редактора/Demo и
resize требуют корректного освобождения mapping, hooks и API-ресурсов. Размер
окна, API и process ID из frame snapshot являются диагностикой, не доказательством
совместимости с любой игрой.
