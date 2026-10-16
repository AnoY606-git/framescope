# FrameScope Vulkan preview layer

Собственный Windows x64 explicit layer. Активен **только** в процессе с basename
`FrameScope.Demo.exe` и валидным `FRAMESCOPE_SESSION` (32 hex digits). Другие процессы
получают обычный forwarding Vulkan chain без подключения mapping и без HUD.
Это preview allowlist, не механизм обхода защиты и не обещание поддержки игр.

## Подключение к родительской сборке

- Sources: `native/vulkan/layer.cpp`; Windows export definition:
  `native/vulkan/FrameScope.Vulkan.def` (экспорты также имеют `dllexport`).
- Output: **`FrameScope.Vulkan.dll`**, без `lib` prefix; скопировать рядом неизменённый
  **`FrameScope.Vulkan.json`**.
- C++17 или новее, Windows x64. Для MSVC использовать `/utf-8`.
- Link static targets: `framescope_common`, ImGui core и **`imgui_vulkan` pinned
  v1.91.9b**. Backend target должен компилироваться с
  `IMGUI_IMPL_VULKAN_NO_PROTOTYPES` (либо `VK_NO_PROTOTYPES`), не только layer.cpp.
- Include paths: `native/include`, pinned Vulkan-Headers `include`, ImGui root,
  ImGui `backends`. Внешний Vulkan SDK пользователю не требуется.
- Нет зависимости от `vulkan-1.lib`: все Vulkan calls используют next GIPA/GDPA.
  Layer использует Kernel32; ImGui core дополнительно использует User32/Shell32
  (clipboard/default shell callbacks), их должен предоставить link toolchain.
  Нет RTSS, kernel driver и глобальной регистрации.

Launcher передаёт child-only environment с каталогом manifest в `VK_ADD_LAYER_PATH`
и `VK_INSTANCE_LAYERS=VK_LAYER_FRAMESCOPE_hud`, сохраняя чужие значения. Если уже
есть `VK_LAYER_PATH`, `VK_ADD_LAYER_PATH` loader игнорирует: это обязан учитывать
launcher. Elevated target не поддержан portable discovery. DLL/JSON должны
существовать весь lifetime процесса. Не копировать loader/DLL в каталог игры.

## Поддержанный renderer path

- Обычный SDR swapchain: `SRGB_NONLINEAR`, RGBA/BGRA 8-bit UNORM или SRGB,
  opaque composite alpha, identity pretransform, одна array layer, 2–16 images.
- Само приложение запрашивает `COLOR_ATTACHMENT` usage; flags/pNext, image count,
  present mode и fullscreen policy не переписываются.
- Queue, переданная приложением в present, должна иметь GRAPHICS capability и
  быть получена через перехваченный `GetDeviceQueue[2]`. Present-only/protected queue
  прозрачно пропускается. После инициализации смена queue не поддержана.
- Один primary swapchain/device. Рекреация с `oldSwapchain` выбирает новое поколение;
  ранее acquired old images всё ещё разрешено present, но без HUD после выбора нового.
- Multiple-swapchain batch, device groups, protected/HDR/shared/stereo, rotating
  surfaces и неизвестные create/present extensions — bypass **до** исходных waits.
  Из present pNext поддержаны present ID, Google timings и maintenance present mode;
  incremental regions и app-owned present fences намеренно пропускаются без HUD.
- Classic render pass с LOAD/STORE в отдельном command buffer; способ rendering
  приложения (render pass/dynamic rendering) от этого не зависит.
- Все исходные binary waits потребляет ровно один HUD submit; downstream present
  ожидает только наш per-image semaphore. Нулевой wait count допустим для корректно
  синхронизированного приложения. Исходные pNext/pResults/массивы не мутируются.
- Before draw проверяется fence **точного slot** round-robin ImGui 1.91.9b, а не
  acquired image index. Если slot занят, пропускается только HUD, не кадр игры.
  Возврат image через acquire разрешает повторный signal per-image semaphore
  лишь благодаря последующему выполнению acquire dependency в application waits.
- Post-submit failure не приводит к повторному present с уже consumed waits.
  Если present не enqueue-ился, приватный semaphore помечается poisoned, без reuse.
- Font atlas загружается один раз на renderer generation. QueueWaitIdle внутри
  pinned font upload допустим только при инициализации. Steady-state idle нет.
- `IniFilename`/`LogFilename` отключены. Нет platform/input backend и файлов ImGui.
  Control mapping читается каждые 500 ms; Enabled и heartbeat старше 3 s отключают
  рисование. FPS — successful/suboptimal **application present rate**, не displayed
  или generated FPS. Данные датчиков и отсутствующие значения не выдумываются.

Bypass/failure reasons выводятся один раз через `OutputDebugStringA` (DebugView или
debugger), не пишутся в игру/реестр/файл и не подменяют application return codes.

## Lifecycle: strict и legacy пути

Layer не включает extensions/features за приложение. Если приложение уже включило
`VK_KHR_swapchain_maintenance1` либо `VK_EXT_swapchain_maintenance1` и соответствующий
feature, к поддержанному present добавляется собственный present fence. Исходный
pNext при этом сохраняется. Эти fences позволяют корректно освобождать presentation
resources, но **не доказывают момент показа кадра на экране**.

Для demo без maintenance используется известная legacy WSI convention: на destroy
ожидаются собственные submit fences и разово `QueueWaitIdle` очереди HUD. Это
практический demo compatibility path, **не универсальное спецификационное доказательство
завершения presentation engine**. Нельзя объявлять по нему сертифицированными все игры.
Первоначальный controlled demo должен завершать/recreate swapchain после своего
device/queue drain; validation и реальный driver test остаются обязательными.

Destroy/recreation может ждать GPU; каждый present этого не делает. При device lost
ожидания прекращаются. Живую DLL нельзя безопасно выгружать вручную: приложение
может хранить адреса layer entrypoints. Отключение HUD означает bypass, не unload.

## Проверки до заявления о runtime support

Локальные повторяемые mock/DLL checks (существующие parent dependencies не меняются):

```powershell
native/vulkan/tests/run_mock_checks.ps1 -BuildDirectory artifacts/build-core
```

Helper компилирует layer и policy tests с `-O3 -DNDEBUG -fstrict-aliasing`: отладочный
неоптимизированный прогон не заменяет Release regression. Root CMake/CTest путь:

```text
cmake --build artifacts/build-core --target FrameScope.Vulkan framescope_vulkan_policy --parallel 4
ctest --test-dir artifacts/build-core -R vulkan --output-on-failure
```

Policy test требует настоящую DLL: adjacent layout используется для `.check`, а
`artifacts/native/tests` читает DLL из sibling `bin`. Путь абсолютный, без поиска
через cwd/PATH; после ошибки загрузки/подписи альтернативные DLL не подбираются.

`-LoaderSmoke` дополнительно запускает собственный console bootstrap против
системного Vulkan loader: instance/device/queue, без окна, swapchain и HUD. Только
новый test process получает layer environment; caller/global environment не меняются.
Все проверочные binaries находятся в игнорируемом `native/vulkan/.check/`.
Команды сборки проверены для llvm-mingw. Helper PS1 синтаксически проверен; если
локальная execution policy запрещает PS1, не ослаблять её: использовать native
compiler/CMake команды. MSVC и повторная GPU HUD проверка Release после исправления
требуют отдельного прогона.
Mock source `.cc` намеренно не попадает в parent `native/vulkan/*.cpp` glob.

`-DemoExecutable <absolute path to FrameScope.Demo.exe>` дополнительно собирает
`hud_smoke.cc` и дважды запускает отдельный demo на 8 секунд. Проверяются реальный
successful HUD submit, Enabled off/on, устаревший heartbeat/восстановление, resize
и продолжение HUD после запроса minimize/restore. Mapping — приватный случайный
session; датчики не симулируются. Это не screenshot/scanout validation.

### Исправление optimized chain reader

В root Release (`Clang 23`, Vulkan-Headers SDK 1.4.363.0, `-O3 -DNDEBUG`) был
воспроизведён сбой исходной проверки `unsupported tail is not lost`. Причина —
чтение typed Vulkan structs как `VkBaseInStructure`: совпадение prefix layout не
делает эти объекты alias-compatible в C++. Неоптимизированные 72 checks его не
обнаруживали.

Теперь все шесть generic pNext walks используют единый `ReadChainHeader` через
`std::memcpy` object representation, без `VkBaseInStructure*` dereference. Payload
читается через фактический ожидаемый тип только после проверки `sType`. Loader
helpers возвращают указатель в исходную live chain, не адрес локальной копии.
Аналогичное чтение первого dispatch word opaque handles также заменено на memcpy.
ABI offsets/sizes известных headers проверяются compile-time assertions.

С теми же Release flags root target `vulkan_layer_policy` прошёл **107 checks**,
включая исходную неизменённую проверку, новые typed link/unlink/relink и sType
изменения, смешанные input/output pNext types, feature payloads, fullscreen tails,
device groups, bounded present-chain cycle и загрузку настоящей root DLL. Ни
оптимизация, ни strict-aliasing, ни проверки не отключались. GPU/demo при этом
повторно не запускались. Финальный CTest повторён 10 раз (`--repeat until-fail:10`),
все прогоны успешны; root policy EXE и root DLL исполнились штатно, без security block.

### Предыдущие runtime evidence (до optimized fix)

На локальном Intel Arc B580 (driver API 1.4.348) ранее проверены:

- llvm-mingw compilation/link с `-Werror` и загрузка настоящей DLL;
- 72 неоптимизированных mock checks (исторический результат, не Release certification);
- настоящий system-loader instance/device/queue lifecycle с explicit layer;
- отдельный Vulkan demo с HUD: 8 секунд, 1045 accepted presents, Enabled off/on,
  resize и запросы minimize/restore, exit 0.

Synchronization validation, MSVC и HUD + exclusive fullscreen этим не сертифицированы.
Отдельные standalone demo fullscreen acquisition checks не являются проверкой
HUD в exclusive fullscreen; интегрированный Release-прогон выполняется отдельно.
Повторный прогон новой ревизии `hud_smoke.exe` со stale-heartbeat сценарием был
заблокирован локальной Application Control signing policy (CodeIntegrity events
3077/3033). Политики не изменялись, обход не выполнялся. Поэтому stale-heartbeat
проверен unit/mock логикой, но его GPU integration run пока не завершён. Для
дальнейшего исполнения нужен штатно разрешённый test build, а не ослабление защиты.

1. Отдельный `FrameScope.Demo.exe` создаёт Vulkan objects штатно, без линковки layer;
   без environment HUD отсутствует, с environment и свежим mapping появляется.
2. Validation chain: FrameScope выше `VK_LAYER_KHRONOS_validation`, чтобы были видны
   дополнительные вызовы; включить synchronization validation. Проверить 2/3 images,
   disable/enable, graph, масштаб, stale heartbeat, resize/minimize/restore/Alt+Tab.
3. Проверить app render pass и dynamic-rendering paths, корректный zero-wait path
   (с host completion), semaphores, повторное acquire и shutdown/recreation.
4. HDR/protected/batch/unsupported pNext должны давать неизменённый present, без HUD.
5. Release сравнить с baseline: overhead и отсутствие per-frame WaitIdle. Ни слой,
   ни PresentMon hook rate не позволяют обещать generated FPS.
6. Exclusive fullscreen подтверждается отдельным successful application-controlled
   `VK_EXT_full_screen_exclusive` acquire, если поддерживается driver, не надписью меню.

## Первичные источники и лицензии

- [Loader/layer interface](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md)
- [Present specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html)
- [Semaphore reuse и legacy teardown limitation](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
- [Present fence semantics](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html)
- [Pinned Dear ImGui backend](https://github.com/ocornut/imgui/blob/v1.91.9b/backends/imgui_impl_vulkan.cpp)
- [Dear ImGui MIT](https://github.com/ocornut/imgui/blob/v1.91.9b/LICENSE.txt)
- [Vulkan-Headers per-file SPDX](https://github.com/KhronosGroup/Vulkan-Headers/blob/main/LICENSE.md)

Backend используется как отдельная permissive dependency; MangoHud/RTSS code не
копировался. Distribution обязана сохранить лицензии фактически закреплённых
dependencies; Vulkan SDK целиком не имеет одной общей MIT-лицензии.
