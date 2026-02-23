# План: увеличить app partition для ESP-IDF прошивки

## Контекст

Flash чип M5StickC Plus = 2MB. Текущая partition table (`partitions_singleapp.csv`) отдаёт app только 1MB, при этом прошивка заполняет 99.3% (1041K из 1048K). Оставшиеся ~960KB flash не используются. Нужно увеличить app partition, чтобы разблокировать добавление функциональности.

## Подход

Создать кастомную partition table с увеличенным app до ~1.9MB. OTA не нужен (заливка по USB). NVS уменьшить до 16KB (WiFi credentials ~2KB, запас достаточный).

### Разметка

| Раздел | Размер | Назначение |
|--------|--------|------------|
| nvs | 16KB (0x4000) | WiFi credentials, настройки |
| phy_init | 4KB (0x1000) | WiFi PHY калибровка |
| factory | ~1936KB (0x1E4000) | Прошивка |

Итого app: **1936KB** вместо 1024KB (+912KB, почти удвоение).

## Файлы для изменения

1. **Создать `partitions.csv`** в корне проекта — кастомная partition table
2. **`platformio.ini`** — добавить `board_build.partitions = partitions.csv`
3. **`sdkconfig.idf.defaults`** — переключить на custom partition table

## Верификация

1. `pio run` — проверить что собирается и flash usage упал до ~54%
2. `pio run --target upload` — залить и проверить загрузку
3. Проверить WiFi подключение (NVS работает)
4. Проверить deep sleep / wakeup
