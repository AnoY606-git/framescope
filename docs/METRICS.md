# Метрики: что означает число

## Кадры

Источник FPS/frametime — временные отметки наблюдаемых **Present-вызовов** в
нативном backend, измеренные QPC. Это не ETW/PresentMon и не момент фактического
показа на мониторе. Вызовы могут ожидать VSync, быть отброшены или не соответствовать
одному видимому кадру. Поэтому показатели называются Present FPS / Present interval.

График содержит до 120 последних interval samples. Первому кадру не предшествует
известный интервал. Невалидные интервалы не должны превращаться в бесконечный FPS.
Текущий `FrameStatistics` усредняет FPS как `1000 × N / sum(interval_ms)` по
последним интервалам, пока сумма не достигнет секунды (либо по всем доступным).
Frametime — последний интервал. История ограничена 60 секундами и 10000 samples.
Lows — обратное среднему самых медленных `ceil(N/100)` и `ceil(N/1000)` интервалов;
1% доступен с 100 samples, 0.1% — с 1000. Значения пересчитываются примерно раз в
250 ms и при достижении порога доступности. Это не определения percentile FPS
других benchmarks; сравнивайте только одинаковые окна и методы.

**Displayed FPS, engine FPS, generated frames и Frame Generation FPS не измеряются.**
Не выводите их как 0 или копию Present FPS. Если такие поля показаны — N/A.

## Система

| Поле | Источник / пределы |
| --- | --- |
| CPU | Windows API; общая загрузка системы, не CPU frame time игры |
| RAM | Windows memory status; использованная/общая физическая память системы, не private bytes Demo |
| GPU load / VRAM | Доступные Windows counters; driver/API/adapter selection могут ограничить данные |
| CPU temperature | Нет гарантированного стандартного источника; N/A без отдельного подтверждённого provider |
| GPU temperature | Read-only WDDM `D3DKMTQueryAdapterInfo`, если драйвер предоставляет значение; 0/невалидное значение означает N/A |
| GPU power | Только при фактически доступном provider; WDDM raw power в 0.1% **не является ваттами** |
| HUD CPU time | Время конкретного измеряемого CPU-участка native HUD; не GPU overhead и не стоимость всего приложения |

`available_mask` отделяет отсутствие источника от настоящего нулевого показания.
Зависший provider не должен оставлять вечное «свежее» значение. Единицы: ms для
interval, µs для HUD CPU, GiB для памяти, °C для температуры, W для мощности.

`FrameScope.WindowsSensors.WddmGpuSensor.ReadAll()` читает adapter performance
через установленный драйвер, без admin и дополнительного Intel SDK. GPU readings
нужно связывать по adapter LUID, а не смешивать датчики нескольких адаптеров.
Подтверждённое чтение одного GPU не гарантирует то же поле на любом Intel/NVIDIA/AMD.

## Advanced Intel: отдельный read-only путь

Подготавливается helper на официальном **Level Zero Sysman**, не IGCL. До
конкретного runtime probe поля остаются условными возможностями. Нельзя заявлять
«все датчики Intel доступны» по наличию headers или имени видеокарты.

| Поля Intel block | Единица | Пример причины N/A |
| --- | --- | --- |
| GPU / memory clock | MHz | Нет соответствующего frequency domain |
| GPU / board power | W | Нет power/energy domain либо ещё нет двух валидных samples |
| Fan / voltage | RPM / V | Драйвер не предоставляет read-only показатель |
| Memory read / write | GB/s | Нет bandwidth counters или недостаточно данных для delta |
| Memory temperature | °C | Датчик/domain отсутствует |
| Throttle flags | Bit flags | API/версия не предоставляет причины throttling |

Общие причины: нет System32 `ze_loader.dll`, не найден подходящий adapter,
unsupported API, отказ доступа, ошибка/устаревший sample. Для каждого отсутствующего
поля фиксируйте N/A **и причину** в диагностике; отсутствие availability bit не равно
нулевой частоте/мощности или отсутствию throttling. Frequency/power/fan setters
не используются. Принудительная установка драйвера и повышение прав не являются
fallback. CPU temperature и displayed/FG FPS этим helper не добавляются.

## Проверка стоимости HUD

Сравнивайте один и тот же Demo/API, разрешение, режим окна, VSync, GPU/драйвер,
power plan и длительность после прогрева. Запишите результаты без HUD и с HUD,
CPU/GPU время, память и условия сбора. Скриншот FPS не доказывает низкий overhead.
Hosted CI, программный renderer и собственный Demo не доказывают ускорение или
совместимость в игре. Числа публикуются только вместе с исходными результатами
и явно указанными условиями в [VERIFICATION](VERIFICATION.md).
Debug предназначен для разработки и может иметь другие оптимизации/проверки;
его overhead не публикуется как производительность Release. Всегда указывайте
configuration и фактически запущенный пакет вместе с результатом.
