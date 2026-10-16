# Совместимость preview

Целевая платформа: **Windows 11 x64**. ARM64, Windows 10, Linux/Wine, macOS и
32-bit процессы не заявлены как поддерживаемые.

| Сценарий | Область preview | Доказательство |
| --- | --- | --- |
| Собственный Demo: DX11 | Основной target | Standalone Demo и подключённый HUD проверяются отдельно |
| Собственный Demo: DX12 | Основной target | Standalone Demo и подключённый HUD проверяются отдельно |
| Собственный Demo: Vulkan | Основной target, explicit layer | Нужен Vulkan runtime; Demo smoke не доказывает корректность HUD layer |
| Windowed / borderless / exclusive | Запрашиваемые режимы Demo | Проверяются отдельно; запрос режима не доказывает фактический exclusive |
| Сторонние игры/приложения | Не обещаны | Нет общей матрицы совместимости |
| CS2 / античиты / защищённые процессы | **Исключены** | Не тестировать внедрение и обход защиты |
| WDDM GPU temperature | При доступности поля у драйвера | Read-only provider, без admin/дополнительного SDK; проверка по adapter LUID |
| Advanced Intel Level Zero Sysman | Отдельный helper в разработке | Каждое поле требует runtime probe и N/A reason; loader не поставляется |
| Intel IGCL | Не используется как MIT-замена | Отдельные ограничительные условия; не заявлен как интегрированный |
| ETW / PresentMon / displayed/FG FPS | Не входят в начальный preview | N/A, не замещать Present FPS |

Поддержка API в исходниках не означает успешную проверку всех его возможностей,
GPU vendors, нескольких адаптеров/мониторов, HDR, high DPI, device removal или
fullscreen transition. Для Vulkan layer не изменяет system/user registry и не
должен включаться для всех приложений пользователя.
Успешный standalone Demo, включая acquisition exclusive mode, не подтверждает
исправность Release HUD/policy и не расширяет поддержку на чужие игры.

Приложению не нужен обязательный admin/kernel service. Если конкретный запуск
требует отключения защит или повышения прав, это не штатный путь preview.
Фактическая матрица результатов и оставшиеся ограничения —
[VERIFICATION](VERIFICATION.md).

Debug и Release — разные файлы/прогоны. Решение Smart App Control для конкретной
Debug DLL не гарантирует загрузку Release DLL или другой сборки. Debug не отключает
SAC и не делает unsigned файл доверенным; [варианты подписи](SIGNING.md) требуют
отдельных внешних предпосылок. Защита Windows не меняется ради запуска.
