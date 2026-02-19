# M5StickC Plus

**SKU:** K016-P
**Источник:** [docs.m5stack.com](https://docs.m5stack.com/en/core/m5stickc_plus)

## Описание

M5StickC Plus — версия M5StickC с увеличенным экраном. Основной контроллер — модуль **ESP32-PICO-D4**, поддерживающий Wi-Fi. В компактном корпусе интегрировано множество аппаратных ресурсов: инфракрасный передатчик, RTC, микрофон, LED, IMU, кнопки, зуммер, PMU и др.

По сравнению с оригинальным M5StickC добавлен **пассивный зуммер (buzzer)**, а экран увеличен до **1.14 дюйма** (135 x 240 TFT, драйвер ST7789v2) — площадь дисплея больше на 18.7% по сравнению с предыдущим экраном 0.96 дюйма. Ёмкость батареи — **120 мАч**. Интерфейс поддерживает продукты серий HAT и Unit.

Компактный инструмент разработки для быстрого прототипирования IoT-продуктов. Подходит даже для начинающих.

## Особенности (Features)

- На базе ESP32, поддержка Wi-Fi
- Встроенный 3-осевой акселерометр и 3-осевой гироскоп (MPU6886)
- Встроенный красный LED
- Встроенный инфракрасный передатчик
- Встроенные часы реального времени (RTC — BM8563)
- Встроенный микрофон (SPM1423)
- 2 пользовательские кнопки, LCD-дисплей (1.14"), кнопка питания/сброса
- Литиевая батарея 120 мАч
- Интерфейс расширения (Grove)
- Встроенный пассивный зуммер
- Носимый и монтируемый форм-фактор

### Платформы разработки

- UiFlow1
- UiFlow2
- Arduino IDE
- ESP-IDF
- PlatformIO

## Комплект поставки

- 1 x StickC-Plus

## Применения

- Носимые устройства
- IoT-контроллеры
- STEM-образование
- DIY-проекты
- Устройства умного дома

## Спецификации

| Параметр | Значение |
|---|---|
| SoC | ESP32-PICO-D4, двухъядерный, до 240 МГц |
| Flash | 4 МБ |
| Wi-Fi | 2.4 ГГц |
| DMIPS | 600 |
| SRAM | 520 КБ |
| Входное напряжение | 5V @ 500mA |
| Интерфейсы | USB Type-C x 1, GROVE (I2C + I/O + UART) x 1 |
| LCD-экран | 1.14", 135 x 240, цветной TFT, ST7789v2 |
| Микрофон | SPM1423 |
| Кнопки | 2 пользовательские кнопки |
| LED | Красный LED x 1 |
| RTC | BM8563 |
| PMU | AXP192 |
| Зуммер | Пассивный, встроенный |
| ИК-передатчик | Есть |
| IMU (MEMS) | MPU6886 |
| Антенна | 2.4G 3D Antenna |
| Внешние пины | G0, G25/G26, G36, G32, G33 |
| Батарея | 120 мАч @ 3.7V |
| Рабочая температура | 0 ~ 60°C |
| Материал корпуса | Пластик (PC) |
| Размер продукта | 48.0 x 24.0 x 13.5 мм |
| Вес продукта | 16.9 г |
| Размер упаковки | 104.4 x 65.0 x 18.0 мм |
| Вес брутто | 24.1 г |

## Распиновка (PinMap)

### Red LED, ИК-передатчик, кнопки, зуммер

| ESP32-PICO-D4 | G10 | G9 | G37 | G39 | G2 |
|---|---|---|---|---|---|
| Назначение | Red LED | IR Pin | Button A | Button B | Buzzer |

### Цветной TFT-экран (ST7789v2, 135 x 240)

| ESP32-PICO-D4 | G15 | G13 | G23 | G18 | G5 |
|---|---|---|---|---|---|
| TFT Screen | TFT_MOSI | TFT_CLK | TFT_DC | TFT_RST | TFT_CS |

### Микрофон (SPM1423)

| ESP32-PICO-D4 | G0 | G34 |
|---|---|---|
| Microphone MIC | CLK | DATA |

### IMU (MPU6886) и PMU (AXP192) — общая шина I2C

| ESP32-PICO-D4 | G22 | G21 |
|---|---|---|
| 6-Axis IMU (MPU6886) | SCL | SDA |
| PMU (AXP192) | SCL | SDA |

### Управление питанием AXP192

| Периферия | Микрофон | RTC | TFT Backlight | TFT IC | ESP32/3.3V/MPU6886 | 5V GROVE |
|---|---|---|---|---|---|---|
| Линия AXP192 | LDOio0 | LDO1 | LDO2 | LDO3 | DC-DC1 | IPSOUT |

### Grove-разъём (HY2.0-4P)

| HY2.0-4P | Чёрный | Красный | Жёлтый | Белый |
|---|---|---|---|---|
| PORT.CUSTOM | GND | 5V | G32 | G33 |

## Включение / Выключение

- **Включение:** нажать кнопку сброса и удерживать минимум **2 секунды**
- **Выключение:** нажать кнопку сброса и удерживать минимум **6 секунд**

## Важные замечания

- Поддерживаемые baud rate: 1200–115200, 250K, 500K, 750K, 1500K
- **G36/G25 делят один порт.** При использовании одного пина второй нужно установить в floating input:
  ```cpp
  setup() {
    M5.begin();
    pinMode(36, INPUT);
    gpio_pulldown_dis(GPIO_NUM_25);
    gpio_pullup_dis(GPIO_NUM_25);
  }
  ```
- Диапазон входного напряжения VBUS_VIN и VBUS_USB ограничен **4.8–5.5V**

## Программное обеспечение

### Arduino

- [StickC-Plus Arduino Quick Start](https://docs.m5stack.com/en/arduino/m5stickc_plus/program)
- [StickC-Plus Arduino Driver Library](https://github.com/m5stack/M5StickC-Plus)
- [StickC-Plus Factory Test Example](https://github.com/m5stack/M5StickC-Plus/tree/master/examples/FactoryTest)

### UiFlow1

- [StickC-Plus UiFlow1 Quick Start](https://docs.m5stack.com/en/uiflow/m5stickc_plus/program)

### UiFlow2

- [StickC-Plus UiFlow2 Quick Start](https://docs.m5stack.com/en/uiflow2/m5stickcplus/program)

### USB-драйвер

- [Скачать FTDI-драйвер](https://ftdichip.com/drivers/vcp-drivers/)

### EasyLoader

- [Скачать EasyLoader StickC-Plus (Windows)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/EasyLoader/Windows/CORE/EasyLoader_M5StickC_Plus_FactoryTest.exe)

### Прочее

- [Восстановление заводской прошивки](https://docs.m5stack.com/en/guide/restore_factory/m5stickc_plus)

## История версий

| Дата | Изменения |
|---|---|
| Первоначальный выпуск | — |
| 2021.12 | Добавлены функции sleep/wake, версия изменена на v1.1 |

## Ресурсы и даташиты

- [Документация продукта (PDF)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/static/pdf/static/en/core/m5stickc_plus.pdf)
- [Схема (Schematic PDF)](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/669/k016-p-StickC-Plus-sche.pdf)
- [3D-модели / Structure Files](https://github.com/m5stack/M5_Hardware/tree/master/Products/K016-P_StickC-Plus/Structures)
- [ESP32-PICO Datasheet](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/669/esp32-pico_series_datasheet_en.pdf)
- [ST7789v2 Datasheet](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/ST7789V.pdf)
- [BM8563 Datasheet (RTC)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/BM8563_V1.1_cn.pdf)
- [MPU6886 Datasheet (IMU)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/MPU-6886-000193%2Bv1.1_GHIC_en.pdf)
- [AXP192 Datasheet (PMU)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/AXP192_datasheet_en.pdf)
- [AXP192 Register Map](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/AXP192_datasheet_cn.pdf)
- [SPM1423 Datasheet (Mic)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/SPM1423HM4H-B_datasheet_en.pdf)
- [Купить на M5Stack Store](https://shop.m5stack.com/products/m5stickc-plus-esp32-pico-mini-iot-development-kit)
- [Сравнение продуктов серии Stick](https://docs.m5stack.com/en/products_selector/m5stick_compare?select=K016-P)
