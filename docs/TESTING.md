# Тестирование

## Автоматические проверки

```powershell
python -m unittest discover -s scripts/tests -v
python scripts/check_repository.py --include-untracked
powershell -NoProfile -File scripts/verify.ps1 -Configuration Release
```

| Набор | Что подтверждает |
| --- | --- |
| Python policy/package tests | Запрет internal/keys/outputs, редактируемые версии зависимостей, безопасный состав source ZIP, точные current-run checksums |
| Synthetic PE tests | Import/delay-import parsing, рекурсивная closure, отказ на отсутствующей DLL/driver/неверной архитектуре |
| Managed tests | Реальные assertions чистого Core; executable harness запускается через `dotnet run`, не через пустой `dotnet test` |
| Native CTest | Зарегистрированные non-GPU native tests; отсутствие тестов — ошибка |
| Build/publish/package audit | Компиляция, версия, legal assets, x64 helpers и наличие необходимых runtime; не runtime-подтверждение renderer |

Пути результатов — `artifacts/test-results/`, включая Python JUnit XML от
`scripts/run_python_tests.py` с фактическими failures/errors/skipped.
`verification.json` создаётся только
после успешного завершения цепочки, но не является подписью и не доказывает
безопасность кода. Policy использует имена Git и ограниченные credential patterns;
это не полноценный secret scanner. Synthetic PE fixtures не запускаются и не
выдаются за настоящие Windows-приложения.

Для настоящих test-SDK проектов `verify.ps1` использует `dotnet test` с TRX;
для текущего исполняемого harness — его exit code и log. Число тестов/пропуски
записывайте из фактического вывода, не из имени файла. До первого commit policy
можно проверить с `--include-untracked`; source archive всё равно берёт только
tracked-файлы.

Debug проверяется отдельно: `./scripts/verify.ps1 -Configuration Debug` использует
`dotnet run/test -c Debug`, свой native cache/CTest и `artifacts/test-results-debug/`.
Его marker — `artifacts/verification-debug.json`; Release marker не подходит.
`-SkipBuild` требует совпадающего publish configuration и фактического cache hash,
а повторная проверка убирает старый marker в backup до начала тестов. Успешный
Debug smoke не считается Release smoke и не доказывает исключение из SAC.

## Настоящий Demo: матрица

Используйте отдельный запуск собственного Demo для каждого DX11, DX12 и Vulkan.
Не используйте CS2 или защищённые процессы. Для каждого прогона запишите:

- Windows build, GPU/adapter, driver/runtime, разрешение, DPI, VSync;
- API и запрошенный/фактический window mode;
- подключение HUD, изменение профиля/layout и обновление Present samples;
- resize, минимизацию/восстановление, закрытие Demo и повторный запуск;
- неверный session/missing helper: объяснимый отказ без fallback к чужому target;
- для Vulkan — layer только у дочернего Demo, без registry изменений;
- отсутствие crash/device loss и фактический exit code.

Launcher-контракт описан в [ARCHITECTURE](ARCHITECTURE.md). Полная сессия требует
созданной mapping; одиночный запуск executable без mapping не считается
успешной интеграцией. Собственный `scripts/smoke_native.py`, если включён в сборку,
должен проверяться по его фактическому CLI и результатам, а не по наличию файла.
Автоматический native smoke не заменяет визуальную проверку HUD.

Windowed, borderless и exclusive проверяются отдельно. Ограниченный hosted
runner может не иметь нужного GPU/Vulkan runtime; такой сценарий отмечается
**не выполнен**, а не зелёным тестом. Multi-monitor, multi-GPU, HDR, device removal
и длительная работа — отдельные проверки, не следствие одного успешного Present.

## Поставка и метрики

Проверьте portable в новой writable-папке без отдельно установленного .NET.
Проверьте installer/update/uninstall под обычным пользователем, отсутствие UAC,
сохранность профилей и отсутствие `portable.txt` в installed mode. Не удаляйте
настройки пользователя автоматически при uninstall.

Стоимость HUD: одинаковый Demo/API и условия, прогрев, HUD off/on, несколько
повторов, исходные результаты. Present FPS не равен displayed/FG FPS;
CPU timing участка HUD не равен GPU overhead. Методика — [METRICS](METRICS.md),
реальные результаты/пропуски — [VERIFICATION](VERIFICATION.md).
