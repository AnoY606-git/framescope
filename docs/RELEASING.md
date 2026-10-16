# Выпуск версии

## До упаковки

`VERSION` — единый источник версии продукта для `Directory.Build.props` и CMake;
`global.json` задаёт SDK baseline. Согласуйте CHANGELOG и документы. Тег
`vVERSION` относится к тому же commit, из которого собраны binary/source assets.
Не заменяйте теги или assets прежнего выпуска: используйте новый номер/тег.

Запустите [TESTING](TESTING.md), запишите фактические результаты и ограничения
в [VERIFICATION](VERIFICATION.md). Отдельно проверьте все заявленные Demo API,
portable и installer. Нельзя объявлять hardware-совместимость по hosted CI.

## Пакеты

```powershell
powershell -NoProfile -File scripts/package.ps1 -Configuration Release
```

Команда выполняет verify, собирает per-user Inno Setup 6 installer и создаёт:

- `FrameScope-VERSION-win-x64-setup.exe`;
- `FrameScope-VERSION-win-x64-portable.zip`, с `portable.txt` только в ZIP;
- `FrameScope-VERSION-source.zip`, только Git-tracked файлы;
- `FrameScope-VERSION-release.json` и `SHA256SUMS` текущей команды.

Выход Release — `artifacts/packages/`. Для отдельного developer preview:

```powershell
./scripts/package.ps1 -Configuration Debug
```

Выход — `artifacts/packages-debug/`; имена содержат **`-debug`**, например
`FrameScope-VERSION-debug-win-x64-portable.zip` и `FrameScope-VERSION-debug-release.json`.
Manifest содержит `configuration: Debug`, `development_build: true`, signing not claimed.
Debug — не stable Release и не benchmark-сборка; Release workflow его отвергает.
Подпись/доверие Windows от Debug configuration не появляются.

Нужен clean committed checkout. `-SkipVerify` допускается лишь с matching
`artifacts/verification.json` либо Debug `artifacts/verification-debug.json`;
проверяются configuration, source state, native cache и hash build-info, package
audit всё равно выполняется. `-PortableOnly`
явно пропускает installer для локальной проверки — это **не полный выпуск**.
`-AllowDirtySource` — только developer-режим: архив по-прежнему tracked-only,
имена получают `-dev`, публикационный workflow такой source state отвергает.
Debug также требует clean committed checkout; dirty developer-пакет появляется
только по явному флагу и называется `VERSION-debug-dev-...`.
До добавления файлов в Git source archive намеренно не создаётся пустым.

Скрипт принимает конкретный publish/installer, не ищет «последний EXE». SHA256SUMS
не включает старые файлы из той же папки. Source ZIP нормализует order/time/modes;
это не заявление побитовой воспроизводимости EXE или ZIP .NET runtime.
Проверьте суммы, состав legal/runtime files и версию `FrameScope.exe`.

## Подпись и installer

Production signing credentials в CI не передаются. Текущий pipeline не подписывает
бинарники и не выдаёт checksum за Authenticode. Preview считается **unsigned / signing
not claimed**, пока фактическая подпись не проверена. Если подпись будет добавлена,
обновите pipeline так, чтобы подписать EXE/DLL/installer **до** окончательных hashes.
Ключи/пароли остаются вне Git, source ZIP, logs и CI artifacts.

Installer устанавливает self-contained приложение в `%LOCALAPPDATA%\Programs\FrameScope`,
`PrivilegesRequired=lowest`, без UAC и регистрации Vulkan layer. Не встраивайте
SDK или графический драйвер. Не устанавливайте поверх portable-папки. Профили
пользователя не удаляются uninstall-скриптом.
Debug installer имеет отдельные AppId/shortcuts и каталог
`%LOCALAPPDATA%\Programs\FrameScope-Debug`; он не обновляет Release installation.
Это не меняет storage-контракт самого приложения: installed builds могут обращаться
к общему per-user storage. Для независимых профилей используйте отдельные portable
папки с `portable.txt` и предварительную копию данных.

Бесплатные варианты с внешними условиями и текущие ограничения описаны в
[SIGNING](SIGNING.md). Signing provider ещё не выбран/не интегрирован; self-signing
не объявляется публичным доверием. Windows security settings не меняются.

## CI и публикация

`.github/workflows/ci.yml` имеет `contents: read`, SHA-pinned Actions, checkout без
сохранения credentials, timeout/concurrency и artifacts на 14 дней. Он выполняет
автоматические тесты/build/package, но не публикует GitHub Release и не подтверждает
hardware-матрицу.
Оба build workflows явно устанавливают per-user Inno Setup 6.5.4 с закреплённым
SHA-256, а `FSC_ISCC` указывает на этот compiler, независимо от версии в runner PATH.
SDK first run не должен генерировать ASP.NET certificate для WPF-сборки.

`.github/workflows/release.yml` запускается **вручную** для уже существующего
`vVERSION`. Read-only job повторно проверяет/собирает tag. Отдельный job с
`contents: write` через environment `release` сверяет manifest/hashes/commit и
создаёт только **draft prerelease** в том же (сейчас private) репозитории.
Настройте required reviewers для environment отдельно; наличие YAML само не
включает защиту environment. Существующий Release не перезаписывается.

Перед ручной публикацией draft приложите release notes с фактическими API/GPU,
ограничениями и unsigned-статусом. Изменение видимости репозитория, tag/push и
публикация — отдельные осознанные действия владельца, не действия package script.

## Лицензии и corresponding source

Сохраняйте LICENSE/NOTICE/авторство, third-party texts и licenses фактических
.NET/native runtimes. Intel IGCL не считается MIT и не добавляется без отдельной
проверки условий/GPL-совместимости.
Если построен `FrameScope.Intel.exe`, он включается как собственный optional
product helper с `native_capabilities.intel_helper`, не как vendor runtime.
Это признак состава пакета, не подтверждение всех Intel API. Перед включением
Level Zero headers добавьте точный pin и применимый MIT notice; `ze_loader.dll`
остаётся частью установленного драйвера, не Release asset.

Архив tracked source соответствует checkout; фиксированные dependency URLs/hash
остаются в CMake-конфигурации. При передаче binary по GPL обеспечьте получателю
**все необходимые Corresponding Source**, включая применимые внешние исходники,
scripts и условия сборки; при необходимости приложите отдельные matching dependency
sources. Одна private-ссылка без доступа получателю не заменяет эту обязанность.
Не объявляйте source ZIP полным offline-набором всех внешних dependencies.
