<!-- SPDX-License-Identifier: GPL-3.0-only -->
# FrameScope.Intel — read-only Level Zero Sysman

Отдельный C++20 / Windows x64 процесс. Нет .NET/IGCL/PresentMon зависимости,
службы, admin, kernel injection, установки драйверов или сетевых запросов.
Собственный код — **GPL-3.0-only**. В игру этот процесс/SDK не внедряется.

## Сборка и лицензии

Только два заголовка upstream **oneapi-src/level-zero v1.34.0**, commit
`934714b278f850d164627eeaaa043eee02785a9e`:

| Файл | SHA-256 |
| --- | --- |
| `include/ze_api.h` | `91735cf4add345459ea48ddd3db60c87d3febf561699ae58945cb92173755c9b` |
| `include/zes_api.h` | `569df31bd158aa479af53c604bc778b41c61e249ad841ea9e977525898991bc4` |
| `LICENSE` | `f2e82047a47ca742088331ae2bc85b97949ed1e97dfa057cdafd03e8709fc0a2` |

Источник: <https://github.com/oneapi-src/level-zero/tree/934714b278f850d164627eeaaa043eee02785a9e>.
Заголовки MIT, оригинальный текст — `LICENSE.level-zero.txt`; включить его в
поставку. Собирать loader/validation layers upstream не нужно. Не добавлять
корневой CMake Level Zero в build: достаточно FetchContent **populate** headers.

Интеграция родительским CMake (этот subtree не меняет корневые файлы):

```cmake
add_executable(FrameScope.Intel native/intel/main.cpp native/intel/sysman.cpp)
target_include_directories(FrameScope.Intel PRIVATE "${level_zero_headers_SOURCE_DIR}/include")
target_link_libraries(FrameScope.Intel PRIVATE framescope_options version)
if(MINGW)
  target_link_options(FrameScope.Intel PRIVATE -static)
endif()
```

Здесь обычный `main`, **не** `wmain`: `-municode` этому target не нужен.
MSVC либо LLVM-MinGW + Windows headers достаточно. Для клиента SDK не нужен:
`ze_loader.lib` не используется, функции загружаются через `GetProcAddress`.

Runtime берётся **только из установленной системы**: абсолютный путь
`GetSystemDirectoryW()/ze_loader.dll`, `LOAD_LIBRARY_SEARCH_SYSTEM32`, затем
проверка реально загруженного пути. Поиск DLL в EXE/CWD запрещён process-local
`SetDefaultDllDirectories`. Сам loader обнаруживает установленный vendor driver.
DLL драйвера, loader и IGCL headers/binaries в поставку не копируются.
IGCL со своими ограничениями лицензии не используется; исключений из GPL не добавлено.

## CLI / интеграция с C#

```text
FrameScope.Intel.exe --once
FrameScope.Intel.exe --watch --interval-ms 500
FrameScope.Intel.exe --once --legacy
```

**На проверенном Arc B580 + 32.0.101.8826 для расширенного мониторинга выбирать
`--watch --legacy`**: standalone на этом драйвере имеет меньше датчиков.
Это не эвристическая подмена метрик: режим/источник присутствуют в каждом JSON.

По умолчанию `--once`, интервал 1000 ms; разрешено 500..5000 ms.
Сначала собирается baseline; первая строка появляется после второго sample.
`--once` — один JSON, `--watch` — по JSON на строку, UTF-8 без BOM.
Watch использует monotonic cadence, а не interval sleep поверх времени polling;
при overrun не делает busy-loop. `intervalMs` — целевой период, counter deltas
считаются по фактическим API timestamps и не подменяются этим периодом.
Штатно stdout содержит только JSON; `--help` — отдельный текстовый режим.
Exit codes: 0 — штатное завершение/закрытие pipe; 2 — аргументы; 3 — backend
недоступен в `--once` (JSON всё равно выдан); 4 — внутренняя ошибка.

Основной путь — `zesInit(GPU_ONLY)` → `zesDriverGet` → `zesDeviceGet`.
`--legacy` — **новый процесс**, `ZES_ENABLE_SYSMAN=1` только в окружении этого
CLI до загрузки runtime; затем `zeInit(GPU_ONLY)` → `zeDriverGet` → `zeDeviceGet`.
Legacy core handles передаются Sysman только при таком legacy-init; standalone
handles никогда не используются для core getters. Если Sysman device properties
в legacy недоступны, допускается core identity getter на **legacy** handle.
Автоматического смешивания
двух init-путей в одном процессе нет. Родитель при необходимости повторяет
запуск с `--legacy`; среда родителя, пользователя и системы не меняется.
Process-local альтернативные/null drivers и loader diagnostic layers отключены:
продукт не принимает фиктивную telemetry через debug environment.

Данные контракта `schemaVersion: 1`:

- `source: "level-zero-sysman"`, UTC `timestamp`, monotonic `monotonicMs`,
  `sequence`, `intervalMs`.
- `backend.status/reason/count`, `mode`, версия системного loader,
  `exports` (наличие символов — **не** гарантия работающего sensor), результаты init.
- `devices[]`: все идентифицированные Intel GPU, не «устройство 0».
  `vendorId/deviceId` — числа, `pci` — domain/bus/device/function или null;
  `id` — PCI address либо **неустойчивый** `session-device-N`.
  `name` может быть generic строкой драйвера. UUID/serial/board ID не экспортируются.
- `capabilities`: результаты enumeration/properties/getters по функции/instance.
- Плоский массив `readings[]`: `key`, `scope`, `instance`, `subdeviceId`, `unit`,
  `value`, `status`, `reason`, `api`, `derived`, `windowMs`, `note`, `flags`.
  Идентичность reading — `(device, key, scope, instance, subdeviceId)`.

В C# читать каждую JSON Line отдельно; число допустимо **только** при
`status == "available" && value != null`. Все прочие состояния — N/A, не 0.
`backend.available` означает доступный backend/идентифицированное GPU,
не наличие всех датчиков. Нельзя переносить старое значение в новый snapshot.
Закрытие stdout reader завершает watch штатно при следующей записи. Ctrl+C/Break
сигналит stop event и прерывает синхронную запись; driver calls остаются
ограничением внешнего vendor runtime, поэтому процесс изолирован от HUD.
После driver reset/hotplug родитель может перезапустить CLI с backoff; device reset
backend не вызывает, индексы между запусками не сопоставлять.

## Метрики и ограничения

| key | scope / единицы / смысл |
| --- | --- |
| `temperature` | `gpu_max`, `memory_max`, `global_max`, board/VR/min/composite — ровно Sysman sensor type; C |
| `frequency.actual`, `frequency.requested` | gpu/memory/media; driver-reported MHz, отрицательный unknown → null |
| `voltage` | gpu/memory/media; V, отрицательный unknown → null |
| `frequency.throttle_reasons` | bitmask + известные имена в `flags`; 0 от успешного API — valid |
| `power.average` | card/package/gpu/memory/stack/unknown; W из delta energy, не TDP |
| `memory.physical_bytes` | device_physical/system_physical, physicalSize от properties; 0/unknown → null |
| `memory.total_bytes`, `.free_bytes`, `.used_bytes` | пара allocatable size/free и их разность, B; не physical capacity |
| `memory.read_bandwidth`, `.write_bandwidth` | B/s из API byte counters / microsecond timestamp |
| `engine.utilization` | готовая aggregate group all/compute/render/media/copy, %, не сумма engine |
| `fan.speed` | RPM **отключён политикой read-only для Windows Sysman**, см. ниже |

`zes_mem_state_t.size` deprecated в новом spec. Значение отдаётся явно как
**driver allocatable size**, с `note`, не как доказанная physical capacity.
Не смешивать used из этой пары с другой physical capacity без отдельной подписи.
Memory frequency драйвер может представлять effective rate в MHz; backend не
превращает 19000 MHz в «физический oscillator 19 GHz» или raw GDDR clock.

Power: microjoules / microseconds = watts. Нужны два успешных snapshot одного
counter; одинаковый timestamp → stale, уменьшение counter/timestamp → новый
baseline (`warming_up`), guessed overflow arithmetic не применяется. Package/GPU
не переименовываются в board power: card — только явный extension/domain либо
`zesDeviceGetCardPowerDomain`. На интегрированных платформах package может
относиться к общему CPU/GPU package; этот scope нельзя объявлять изолированной
мощностью GPU chip. Отдельный CPU backend здесь не реализован.

Bandwidth: поля API уже **bytes**. Повторное x32 не применяется; преобразование
PMT transactions в bytes делает Intel BMG implementation. Источник:
<https://github.com/intel/compute-runtime/blob/68d8e91bb2885646da5f77dbd1cd7ffa211ea0b7/level_zero/sysman/source/shared/windows/product_helper/xe2_hpg_core/bmg/sysman_product_helper_bmg.cpp>.
Reset/rollover не выдумывается: прерывается интервал и прогревается новый baseline.
Legacy Windows implementation может занулить отдельное поле при ошибке KMD
response и при этом вернуть SUCCESS. Поэтому нулевой cumulative byte counter
не объявляется valid 0 B/s, пока этот конкретный counter хотя бы раз не стал
ненулевым. Причина N/A — `zero_counter_without_validity_flag`. После доказанного
ненулевого baseline неизменившийся counter даёт настоящий 0 B/s.

Engine timestamps implementation-specific: используется только отношение
`delta(activeTime)/delta(timestamp) * 100`, **не** перевод в ms.
`windowMs` поэтому null. Root aggregate group не заменяется суммой single groups;
ratio вне 0..100 считается invalid_data, не clamp к красивому значению.

### Почему fan getters не вызываются

В upstream Windows `WddmFanImp::getProperties` содержит внутренний
`KmdSysman::Command::Set` для `CurrentNumOfControlPoints`; также выбор fan в
query может менять controller selection. Поэтому даже enumeration/getters
могут не удовлетворять обещанию read-only. Разрешённые query exports видны в
диагностике, но **ни enum, ни fan getter финальный backend не вызывает**.
Причина N/A: `read_only_policy_windows_sysman_fan_getters_can_write`.

Источник (зафиксированный audit commit):
<https://github.com/intel/compute-runtime/blob/68d8e91bb2885646da5f77dbd1cd7ffa211ea0b7/level_zero/sysman/source/api/fan/windows/sysman_os_fan_imp.cpp>.
При раннем exploratory probe установленный `zesFanGetProperties` был вызван до
этого аудита и вернул через fan API 27 RPM. Это число не считается проверенным;
выполняет ли установленный binary внутренние upstream-записи, не установлено.
«Восстановительных» аппаратных записей не делалось; в финальном snapshot RPM null.

## Реальный probe этой машины

Windows 11 Pro build 26300, Arc B580 driver 32.0.101.8826, обычный пользователь,
HVCI running. `ze_loader.dll` из System32 **1.28.2.0**.

Standalone init и Intel GPU enumeration работают; PCI `0000:03:00.0`,
vendor `0x8086`, device `0xe20b`; имя API `Intel(R) Graphics [0xe20b]`.
В первом baseline probe получены GPU actual/request 400 MHz, GPU voltage
0.745 V, memory actual 19000 driver MHz, media 400 MHz, physical memory
12884901888 B (12 GiB), allocatable 12706643968 B, used около 1.2 GB,
aggregate engine percentages. Это мгновенные показания, не калибровка/benchmark.

**Точный пробел standalone Sysman на этом драйвере**:

- temperature enumeration успешно возвращает **0 domains**;
- package/card `zesPowerGetEnergyCounter` → `ZE_RESULT_ERROR_UNSUPPORTED_FEATURE`;
- `zesMemoryGetBandwidth` → `ZE_RESULT_ERROR_UNSUPPORTED_FEATURE`;
- memory requested frequency и voltage → unknown negative sentinel.

**Отдельный legacy-init на той же системе** реально даёт:

- имя `Intel(R) Arc(TM) B580 Graphics`;
- GPU max 58 C, memory/global max 62 C в проверенном snapshot;
- card power около 22.76 W из двух energy samples;
- те же частоты/память/voltage/engine groups;
- memory bandwidth API возвращает SUCCESS, но оба raw counter остаются 0.
  В финальном backend это **N/A**, а не выдуманный 0 B/s: поле пока не доказано.

Итоговые ограничения legacy: GPU-chip power отдельно от card не экспонируется;
memory requested clock/voltage unknown; fan выключен политикой; bandwidth
пока unverified. Значения температур/мощности выше — результаты реального
однократного read-only probe, не константы программы.

Никакого IGCL workaround/license exception не добавлено. Родительский настоящий
D3DKMT temperature backend может отдельно дать температуру с собственным source;
этот CLI не присваивает чужие данные Sysman и не выдаёт fake temperature fallback.

Финальный безопасный stdout сохраняется родителем/проверкой в
`artifacts/verification/intel-probe.json`; дополнительные артефакты сборки
создаются только в approved temporary directory.

## Проверки

`tests/telemetry_tests.cpp` — отдельный C++20 executable без Level Zero/Windows:
единицы power/bandwidth, warmup/reset/stale, настоящий 0, запрет invalid percent,
JSON escaping/nonfinite guard и отсутствие cached numeric value у unsupported.
Самостоятельная сборка: compile этот файл и запустить executable.

`tests/shutdown_tests.cpp` — Windows executable, аргументы: путь к CLI и
опциональный `--legacy`. Для LLVM-MinGW нужен `-municode -luser32` именно
этому **тестовому** target. Проверяет closed stdout pipe, Ctrl+Break, Ctrl+C.
Console signals направляются в отдельную приватную test console, не в console
пользователя/harness. Обе init-ветки прошли четыре реальных subprocess-теста,
включая прерывание записи в заполненный, не читаемый родителем stdout pipe.

`tests/cli_smoke.ps1` — воспроизводимый JSON/arguments/watch smoke test.
Текущая системная PowerShell policy блокирует запуск `.ps1`; policy не менялась.
Синтаксис проверяется parser API; эквивалентные native CLI проверки выполнены
непосредственными shell-командами: обе init-ветки, шесть invalid arguments,
null/finite/privacy/fan invariants, неизменность окружения родителя. Отдельно
проверены по три JSON watch-строки с sequence 1/2/3 в обоих режимах.

PE imports проверены: только Windows/UCRT/Version, без loader import library,
IGCL и сторонних C++ runtime DLL. Live probe не запускает игры;
setter/reset API отсутствуют в call paths.

## Последний статус проверки / App Control

Сохранённый реальный snapshot от `2026-10-16T15:52:55.905Z`:
GPU 56 C, memory/global max 60 C, card power 22.596884330546 W,
physical memory 12884901888 B; 21 available reading и 6 честных N/A.
Это **доказательство прежнего запуска**, не источник live fallback.

Отдельный steady-cadence watch test: 6 строк, интервалы между timestamp
1000/1015/1000/1000/985 ms, 16.19 s включая startup, CPU процесса 250 ms,
peak working set около 90.64 MiB. Это не замер overhead игры/DWM/GPU;
startup требует отдельного budget, для reader разумен timeout 30 s.

После следующей необязательной пересборки Windows **заблокировала запуск**
нового EXE до входа в main: `CreateProcess` error **4551**, официальное имя
`ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION` — "An Application Control policy has
blocked this file". Источник определения:
<https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/shared/winerror.h>.

Почему предыдущая сборка допускалась, а новая нет, не установлено. Политики,
HVCI и blocklist не менялись; обхода путём переименования/переноса не делалось.
Необязательная оптимизация identity-query возвращена к уже проверенной
последовательности API; новый binary после блокировки не объявляется runtime-tested.
Для повторной проверки нужна доверенная/подписанная сборка, разрешённая
владельцем App Control policy, **не отключение защиты**.

Родительский C# reader должен отдельно обработать launch error 4551 как N/A
live source. Сам CLI не может выдать JSON, если ОС не позволила создать процесс.
Нельзя подставлять `intel-probe.json` или старые числа вместо живой telemetry.
