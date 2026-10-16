# Проверки 0.1.0-preview

Срез локальной квалификации: **16.10.2026**. Это не сертификат совместимости
со всеми играми. Более ранний успешный прогон не делает проверенной новую
пересборку: конфигурация и фактические блокировки ниже указаны отдельно.

## Среда

- Windows 11 Pro x64, build 26300; Ryzen 5 7600, 6 ядер / 12 потоков, 32 GB RAM.
- Intel Arc B580, driver **32.0.101.8826**; дисплей 2560×1440.
- Системный Vulkan API 1.4.348; Level Zero loader 1.28.2.0.
- .NET SDK **10.0.402** / runtime **10.0.13**.
- LLVM-MinGW 20261006, Clang 23.1.3; CMake 3.31.8, Ninja 1.12.1.
- Обычный пользователь. HVCI работает, Smart App Control включён.
  Политики защиты, глобальный Vulkan environment и драйверы не изменялись.

## Фактические результаты

| Проверка | Результат | Область доказательства |
| --- | --- | --- |
| Managed Release build | 0 warnings / 0 errors | WPF, Core, WindowsSensors; не доказывает запуск native DLL |
| Managed behavioral checks | 85/85 в Release и Debug | Настройки, атомарное сохранение, CPU/GPU counters, seqlock, Intel JSON/PCI/freshness/identity |
| Python tooling | 63 passed / 1 skipped | Включает реальный Inno version probe; symlink запрещён хостом, synthetic reparse-ancestor проверка пройдена |
| Native compilation | Debug и первоначальный Release собраны | Не приравнивается к GPU execution |
| Native statistics / IPC | Passed | Формулы low, лимиты истории, warmup, torn/stale snapshots, ABI |
| CPU-only HUD layout / fonts | Passed | Четыре роли шрифта, idempotence, геометрия panel/strip; без GPU |
| Vulkan policy regression | 107 checks в оптимизированной сборке; затем Debug passed | Исправлено чтение pNext без strict-aliasing UB; последующие запуски отдельно блокировались SAC |
| Intel unit checks | Passed | Единицы, reset/warmup, status, JSON и отсутствие выдуманных значений |
| Standalone Demo | 17/17 | DX11/DX12/Vulkan, gate, resize/minimize/restore, borderless, invalid arguments |
| Standalone exclusive | Получен на всех трёх API | DXGI GetFullscreenState и успешный Vulkan extension acquire; не просто fullscreen-стиль окна |
| DX11 HUD visibility fix | Release 20/20 и Debug 12/12 отдельных снимков | Убрано чередование HUD/пустого кадра из-за лишнего completion-query gate; без принудительного Flush/Wait |
| Debug native HUD, первая интеграция | 8 s на каждом API, exit 0 | DX11 1125, DX12 834, Vulkan 1130 accepted presents; счётчик совпал с Demo |
| Переработанный HUD | Реальные DX12/Vulkan снимки и exit 0 | 1132 / 1129 presents за 8 s; один запуск DX11 заблокирован до старта с 4551 |
| Новый strip HUD в exclusive | 3/3, exit 0 | DX11 849/849, DX12 849/849, Vulkan 851/851 Demo/HUD presents; actual exclusive acquired, видимость подтверждена PNG |
| Live Intel Debug probe | Exit 0, legacy mode | GPU 57 C, VRAM 60 C, card 23.12 W, GPU 400 MHz, memory 19000 driver-reported effective MHz, physical VRAM 12 GiB; это разовый snapshot, не константы |
| Live WPF controller Intel pipeline | Exit 0, Starting → Live | Точный PCI 0000:03:00.0/LUID match; availableMask 811, card 23.08 W, GPU 56 C; настоящий helper stdout, без файлового fallback |
| Self-contained publish | Собран | Финальный аудит состава пакета и установщика фиксируется отдельно |
| Install / update / uninstall | Не проверено | Компиляция setup не заменяет установку на чистой системе |
| Game overhead budget | Не измерен | Debug и короткий VSync smoke не являются сравнительным benchmark |
| Доверенная подпись | Не получена | Локальный Debug не является доверенно подписанным кодом |

Локальные необработанные evidence находятся в игнорируемом
`artifacts/verification/`: `demo-standalone.json`, `debug-hud.json`,
`redesign/native-smoke.json`, `intel-debug-live.json` и настоящие PNG-снимки.
Сохранённый sensor snapshot используется только как свидетельство проверки —
никогда как источник live fallback.

## Smart App Control: ограничение среды

Некоторые свежие EXE/DLL Windows отклоняла с **4551 / 0x800711C7**;
CodeIntegrity 3077/3033 указывал на VerifiedAndReputableDesktop. В последнем
Debug CTest статистика, layout и Intel unit checks прошли, а новый executable
Vulkan policy не был запущен ОС. Это **Not run / blocked**, не логическая ошибка
теста и не успешный прогон. Ранние полные успешные наборы перечислены выше.

Debug/Release не создают исключения SAC. Защита не отключалась, файлы ради обхода
не переименовывались, старые разрешённые бинарники не подставлялись вместо новых.
Требуется штатное доверие подписи либо отдельно разрешённая тестовая среда.

## Пределы

Hosted CI подтверждает только завершившиеся команды; он не подтверждает реальную
GPU-матрицу. Demo не равен чужой игре: модуль preview ограничен собственным
`FrameScope.Demo.exe`. CS2, сетевые игры и обход античитов не тестировались.
HDR, linked/multi-GPU, длительная работа и произвольные чужие swapchain не
сертифицированы. Displayed/engine/FG FPS и CPU temperature не заявляются.

Intel telemetry — **Level Zero Sysman, не IGCL**. Fan getters отключены из-за
потенциальной внутренней записи Windows driver implementation; bandwidth с
неподтверждёнными нулевыми счётчиками остаётся недоступным. Подробности и ранний
exploratory getter-вызов описаны в `native/intel/README.md` исходного дерева.
