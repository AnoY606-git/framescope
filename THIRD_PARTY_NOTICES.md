# Сторонние компоненты

Лицензия собственного кода FrameScope — GPL-3.0-only. Она не переименовывает
лицензии зависимостей и не делает системные библиотеки собственностью проекта.

| Компонент | Использование | Условия / источник |
| --- | --- | --- |
| Dear ImGui | Native HUD и официальные renderer backends; commit `f5befd2d29e66809cd1110a152e375a7f1981f06` | MIT, Omar Cornut; [исходники](https://github.com/ocornut/imgui), [текст](docs/licenses/ImGui-LICENSE.txt) |
| MinHook | DX hooks внутри собственного Demo; commit `c3fcafdc10146beb5919319d0683e44e3c30d537` | BSD-2-Clause, Tsuda Kageyu; включает HDE, Vyacheslav Patkov; [исходники](https://github.com/TsudaKageyu/minhook), [полный текст](docs/licenses/MinHook-LICENSE.txt) |
| Vulkan-Headers | Определения Vulkan; `vulkan-sdk-1.4.363.0` | Apache-2.0 / MIT по конкретным файлам, Khronos Group; [исходники](https://github.com/KhronosGroup/Vulkan-Headers), [уведомление](docs/licenses/Vulkan-Headers-LICENSE.md) |
| Level Zero headers | Read-only Intel Sysman provider; v1.34.0, commit `934714b278f850d164627eeaaa043eee02785a9e` | MIT, Intel Corporation; [исходники](https://github.com/oneapi-src/level-zero), [текст](docs/licenses/Level-Zero-LICENSE.txt) |
| .NET / WPF | Самодостаточный managed runtime | MIT и собственные third-party notices дистрибутива Microsoft; [runtime](https://github.com/dotnet/runtime), [WPF](https://github.com/dotnet/wpf) |
| Windows SDK / графические драйверы | Системные API; не копируются из System32 в поставку | Условия соответствующего производителя; это не компоненты под MIT FrameScope |
| C/C++ runtime, если динамический | Зависимости фактически собранного native PE | Условия конкретного toolchain; упаковка требует соответствующие notices |
| LLVM libc++ / compiler-rt, при LLVM-MinGW build | Статически встроенные runtime portions; compiler executable не поставляется | Apache-2.0 WITH LLVM-exception; [Apache](docs/licenses/Apache-2.0.txt), [exception](docs/licenses/LLVM-exception.txt), [upstream](https://llvm.org/LICENSE.txt) |

Корневой `CMakeLists.txt` определяет фактические версии native-зависимостей.
Обновляйте эту таблицу и тексты лицензий при их изменении. Упаковка добавляет
лицензии фактически выбранных .NET runtime packages и переданные notices native
runtime; `native-runtime.json` записывает найденные DLL и SHA-256, не личные пути.

Intel Graphics Control Library (IGCL) имеет **отдельные условия Intel**, не
общую MIT-лицензию, и здесь не используется. Провайдер использует MIT headers
Level Zero, динамически обращаясь к установленному системному runtime.
`ze_loader.dll`, driver DLL и SDK tools не входят в поставку. Наличие Intel GPU
не означает доступность всех полей Sysman.

Для advanced Intel выбран отдельный read-only путь Level Zero Sysman, чьи
официальные API headers имеют MIT-условия. Это не смена лицензии IGCL. Точный
header pin и применимый license text добавляются при выборе/включении зависимости;
работа всех API и GPU не заявляется заранее. Driver-provided `ze_loader.dll`
из System32 не включается в пакет.

LLVM embedding exception относится к соответствующим runtime portions, попавшим
в object code при компиляции; она не меняет GPL-3.0-only собственного кода.
Прочие части toolchain сохраняют свои уведомления. Static linking не означает
отсутствие стороннего кода, даже если PE imports содержат только Windows DLL.

ETW/PresentMon и сторонние игровые/античит-компоненты в начальную область
preview не включены. Inno Setup используется как внешний инструмент сборки;
его compiler не входит в установленное приложение.
