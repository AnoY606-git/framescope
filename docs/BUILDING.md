# Сборка

## Инструменты

| Компонент | Требование |
| --- | --- |
| ОС / процесс shell | Windows x64; целевой runtime — Windows 11 x64 |
| PowerShell | Windows PowerShell 5.1 или PowerShell 7 |
| .NET SDK | .NET 10, baseline/roll-forward из `global.json` |
| C++ | Настроенный x64 MSVC + Windows SDK либо проверенный Windows LLVM-MinGW toolchain |
| CMake / Ninja | CMake 3.25+ и Ninja в PATH или через переменные ниже |
| Python | 3.10+, только standard library для repository/package tests |
| Inno Setup | CI закрепляет **6.5.4**; локально поддерживается 6.3+ семейства 6 |

`VERSION` — единственный источник версии продукта, сейчас `0.1.0-preview`;
его читают `Directory.Build.props` и CMake. Не
создавайте независимый «номер релиза» в installer. Версия фактически применённого
SDK и compiler записывается в `build-info.json`.

## Локальные инструменты

```powershell
powershell -NoProfile -File scripts/bootstrap.ps1
```

Команда проверяет .NET/Python и сообщает остальные требования. Ничего глобально
не устанавливается. При осознанном выборе можно загрузить точный SDK ZIP из
официальных Microsoft release metadata в `.tools/dotnet`:

```powershell
powershell -NoProfile -File scripts/bootstrap.ps1 -InstallDotNet -InstallInno
```

SHA-512 проверяется **до** распаковки; metadata получены по HTTPS, это не отдельная
подпись архива. Глобальный PATH/registry не меняются, существующий toolchain не
перезаписывается. SDK, compiler, CMake, Ninja и Inno Setup не входят в Git.
Не меняйте machine execution policy или защиту Windows ради сборки; учитывайте
локальную политику запуска скриптов своей среды.

`-InstallInno` получает официальный Inno Setup **6.5.4** с GitHub release
`jrsoftware/issrc/is-6_5_4`; фиксированный SHA-256 хранится в `scripts/common.ps1`
и проверяется до исполнения. Установка — `.tools/inno-6.5.4`, `/CURRENTUSER`,
без ярлыков и изменения PATH. Возможна per-user запись деинсталляции Inno;
machine-wide установка не выполняется. Существующий каталог не перезаписывается,
несовпадающий cached installer требует проверки, а не автоматической замены.
В CI выбранный `FSC_ISCC` передаётся следующему шагу через `GITHUB_ENV`, поэтому
Inno 7 из runner PATH не подменяет закреплённый compiler.

Скрипты поддерживают process-local overrides:
`FRAMESCOPE_DOTNET`, `FRAMESCOPE_PYTHON`, `FRAMESCOPE_CMAKE`, `FRAMESCOPE_CTEST`,
`FRAMESCOPE_NINJA` и `FSC_ISCC` — полные пути к executable; прежний
`FRAMESCOPE_ISCC` поддерживается как fallback. Для собственного
compiler используйте `CC`/`CXX` и при необходимости CMake toolchain file.
Из обычного shell `build.ps1` умеет подключать уже установленные VS C++ tools
**только к текущему процессу**, без изменения глобального окружения. Явные
`CC`/`CXX` имеют приоритет, затем готовый `cl` environment, затем установленный
MSVC через vswhere. Только при отсутствии VS выбирается пара gcc/g++ или clang;
случайный g++ в PATH не переключает CI с установленного MSVC.
`DOTNET_GENERATE_ASPNET_CERTIFICATE=false` задан в scripts/CI, чтобы SDK first run
не создавал не относящийся к WPF dev certificate.

## Сборка и проверка

```powershell
powershell -NoProfile -File scripts/build.ps1 -Configuration Release
powershell -NoProfile -File scripts/verify.ps1 -Configuration Release -SkipBuild
```

`verify.ps1` без `-SkipBuild` сначала выполняет полный build. Без `-Configuration`
используется Release; существующие Release-пути сохранены.

| Результат | Release | Debug |
| --- | --- | --- |
| Native build tree | `artifacts/native/` | `artifacts/native-debug/` |
| Native helpers | `artifacts/native/bin/` | `artifacts/native-debug/bin/` |
| Self-contained app | `artifacts/publish/` | `artifacts/publish-debug/` |
| Проверки/отчёты | `artifacts/test-results/` | `artifacts/test-results-debug/` |
| Verification marker | `artifacts/verification.json` | `artifacts/verification-debug.json` |
| Пакеты | `artifacts/packages/` | `artifacts/packages-debug/` |
| Installer build | `artifacts/installer/` | `artifacts/installer-debug/` |

В каждом publish `FrameScope.exe` находится в корне, helpers — в его `native/`.
Сборка для разработки:

```powershell
./scripts/build.ps1 -Configuration Debug
./scripts/verify.ps1 -Configuration Debug -SkipBuild
./scripts/package.ps1 -Configuration Debug -SkipVerify
```

Это не обещание доверия SAC и не изменение настроек Windows. Debug предназначен
для диагностики; его timing/overhead нельзя выдавать за benchmark Release.
Для текущего локального LLVM-MinGW используется static libc++/compiler-rt;
Microsoft debug CRT DLL не распространяются. Иной toolchain требует отдельной
проверки runtime/лицензий; упаковка отвергает `ucrtbased.dll` и подобные debug CRT DLL.

`-NativeBuildDirectory` позволяет выбрать существующий Ninja cache. Скрипт читает
из него **CMAKE_BUILD_TYPE** и **FRAMESCOPE_NATIVE_OUTPUT_DIRECTORY**, а не копирует
Release bin по фиксированному пути. Например, для собственного cache:

```powershell
./scripts/verify.ps1 -Configuration Debug -NativeBuildDirectory artifacts/build-debug
./scripts/package.ps1 -Configuration Debug -NativeBuildDirectory artifacts/build-debug -SkipVerify
```

Относительные пути параметров считаются от корня репозитория. Для нового tree
output по умолчанию — его `bin/`; можно задать `-NativeOutputDirectory` либо
совпадающее `-DFRAMESCOPE_NATIVE_OUTPUT_DIRECTORY:PATH=...` в `-CMakeArguments`.
Остальные CMake definitions передаются дальше. Конфликт configuration/output с
существующим cache останавливает сборку: выберите новый tree или сначала осознанно
перенастройте cache. Через CMakeArguments нельзя подменять `-S/-B/-G` и смешивать
один tree/output между Debug и Release.

`build-info.json` записывает configuration, фактический cache hash и source state.
Verification marker относится к тому же configuration/source/build-info, не к
любому бинарнику с похожим именем. После пересборки/изменения cache нужен новый verify.

Скрипт не переиспользует старое содержимое publish: предыдущая папка сохраняется
как соседняя `publish[-debug]-previous-*`. Удаляйте ненужные локальные результаты вручную
после проверки. Не запускайте несколько сборок в одном build tree одновременно.
Разные native trees не делают параллельный .NET restore/publish в одном checkout
безопасным: часть `obj`/restore-файлов общая. Debug и Release managed-сборки
выполняйте последовательно.

## Native runtime и лицензии

MSVC по умолчанию запрашивается с static CRT. Если выбранный toolchain всё же
требует динамические runtime DLL, `native_runtime.py` разбирает normal/delay PE
imports и рекурсивно копирует только найденную closure. System32, driver DLL,
Vulkan loader и `ze_loader.dll` не «подбираются» с компьютера. Отсутствующая DLL останавливает
сборку, а не выдаёт неполный пакет за готовый.

```powershell
./scripts/build.ps1 -NativeRuntimeSearchPath 'X:\local-toolchain\bin' `
    -NativeRuntimeNoticesPath 'X:\local-toolchain\licenses'
```

Эквиваленты: `FRAMESCOPE_NATIVE_RUNTIME_DIRS` и `FRAMESCOPE_NATIVE_NOTICES_DIR`
(несколько каталогов через `;`). Передавайте именно разрешённые к распространению
runtime и их юридические уведомления, не папку Windows/драйвера. Из exact NuGet
runtime packages отдельно копируются .NET/WPF licenses/notices.
`FrameScope.Intel.exe`, если построен, является собственным product helper,
не сторонним runtime. Его наличие записывается как `native_capabilities.intel_helper`;
это не подтверждение доступности Intel API или показаний на конкретном GPU.

## Частые проблемы

- SDK не найден: проверьте `global.json`, текущий каталог и выбранный `dotnet.exe`.
- Ninja/cl не найден: используйте x64 developer shell или явные local tool paths.
- Другой generator/compiler: выберите отдельный `-NativeBuildDirectory`, не
  переиспользуйте CMake cache от другого toolchain.
- Не хватает Vulkan runtime на пользовательском ПК: нужен совместимый драйвер,
  не DLL с неофициального сайта. Build headers не заменяют runtime.
- Source/package policy не проходит: исправьте конкретный путь/версию/уведомление;
  checker не печатает найденные значения credentials.
