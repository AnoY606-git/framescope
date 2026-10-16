# Изменения

## 0.1.0-preview — в разработке

Первый preview: WPF-редактор, общие настройки/профили и нативный HUD с собственным
Demo для DX11, DX12 и Vulkan. Реализация и проверка выполняются отдельно;
этот список задаёт область выпуска, а не подтверждает готовность всех функций.

- Один процесс Demo и одна сессия HUD; компактная и обычная компоновки.
- Метрики Present-интервалов; недоступные системные показания — N/A.
- Read-only WDDM GPU temperature при поддержке драйвера; advanced Intel Level Zero
  Sysman helper разрабатывается отдельно, без обещания всех полей и без setters.
- Самодостаточная Windows x64 поставка: portable ZIP и per-user EXE installer.
- Отдельный Debug developer preview: изолированные build/publish/package paths,
  configuration-specific verification и явные `-debug` имена, без изменения защиты Windows.
- CS2, античиты, ETW/PresentMon, displayed/Frame Generation FPS и обязательные
  kernel-компоненты в preview не входят.

Дата публикации, итоговые изменения и фактическая проверка будут добавлены перед
выпуском. Результаты — в [VERIFICATION](docs/VERIFICATION.md).
