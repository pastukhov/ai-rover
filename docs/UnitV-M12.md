# UnitV-M12

**SKU:** U078-V-M12
**Производитель:** M5Stack
**Цена:** ~$39.90 USD
**Документация (обновлена):** 2026-02-04

---

## Описание

Unit V-M12 — AI-камера с объективом стандарта M12 и процессором Kendryte K210. Интегрирует двухъядерный 64-битный RISC-V CPU и нейросетевой процессор (KPU) в одном чипе.

Камера OV7740 (wide-angle) соответствует стандарту M12 и поддерживает замену на другие M12-совместимые объективы. На корпусе — две программируемые кнопки, слот microSD, разъём HY2.0-4P (Grove) и USB Type-C.

Компактный форм-фактор позволяет встраивать модуль в различные устройства. Поддерживает распознавание объектов в реальном времени (размер, координаты, тип цели), свёрточные нейросети (CNN) на edge-устройстве.

---

## Характеристики (Features)

- Dual-core 64-bit RISC-V RV64IMAFDC (RV64GC) CPU / 400 MHz
- Double-precision FPU
- 8 MiB 64-bit on-chip SRAM
- Neural Network Processor (KPU) / 0.8 TOPS
- Programmable I/O Array (FPIOA) — 48 свободных пинов, 255 внутренних функций
- AES, SHA256 Accelerator
- Direct Memory Access Controller (DMAC)
- FFT Accelerator
- MicroPython (MaixPy) Support
- Firmware Encryption Support
- Бортовые ресурсы:
  - Flash: 16M
  - Камера: OV7740
  - Кнопки: 2 шт.
  - Слот расширения: microSD card
  - Интерфейс: HY2.0-4P / совместим с GROVE

---

## Спецификации

| Параметр | Значение |
|---|---|
| Процессор | Kendryte K210 — Dual-core 64-bit RISC-V RV64GC / 400 MHz |
| SRAM | 8M (64-bit on-chip) |
| Flash | 16M |
| Питание | 5V @ 500mA |
| KPU Neural Network Size | 5.5M–5.9M |
| Интерфейсы | Type-C x 1, HY2.0-4P (I2C+I/O+UART) x 1 |
| Кнопки | Custom Buttons x 2 |
| Камера | OV7740 (M12 specification, wide-angle) |
| FOV | 80° |
| Внешнее хранилище | TF Card / microSD |
| Материал корпуса | Plastic (PC) + CNC Metal (Aluminium) |
| Размер продукта | 40.0 x 24.0 x 16.4 мм |
| Вес продукта | 13.4 г |
| Размер упаковки | 70.0 x 50.0 x 30.0 мм |
| Вес брутто | 20.0 г |

---

## PinMap

| UnitV | G8 | G19 | G18 | G34, G35 |
|---|---|---|---|---|
| Hardware | RGB LED | Button A | Button B | — |
| HY2.0-4P | — | — | — | Interface (UART) |

- **G8** — RGB LED
- **G19** — Button A
- **G18** — Button B
- **G34, G35** — HY2.0-4P Grove-разъём (UART TX/RX для связи с хост-устройством)

---

## Совместимость microSD карт

Unit V не поддерживает все типы microSD. Результаты тестирования:

| Бренд | Объём | Тип | Скорость | Формат | Результат |
|---|---|---|---|---|---|
| Kingston | 8G | HC | Class4 | FAT32 | OK |
| Kingston | 16G | HC | Class10 | FAT32 | OK |
| Kingston | 32G | HC | Class10 | FAT32 | **NO** |
| Kingston | 64G | XC | Class10 | exFAT | OK |
| SanDisk | 16G | HC | Class10 | FAT32 | OK |
| SanDisk | 32G | HC | Class10 | FAT32 | OK |
| SanDisk | 64G | XC | Class10 | — | **NO** |
| SanDisk | 128G | XC | Class10 | — | **NO** |
| XIAKE | 16G | HC | Class10 | FAT32 | OK (Purple) |
| XIAKE | 32G | HC | Class10 | FAT32 | OK |
| XIAKE | 64G | XC | Class10 | — | **NO** |
| TURYE | 32G | HC | Class10 | — | **NO** |

> Рекомендуется использовать карты HC (до 32 ГБ) с FAT32. XC-карты (64 ГБ+) часто не поддерживаются.

---

## Комплект поставки

- 1x UnitV-M12
- 1x HY2.0-4P Grove Cable (20 см)

---

## Применения

- Object Detection / Classification
- Real-time Acquisition of Target Size and Coordinates
- Real-time Acquisition of Detected Target Types
- Shape Recognition
- Video Recording

---

## Процессор Kendryte K210

### Архитектура
- Двухъядерный 64-bit RISC-V RV64IMAFDC (RV64GC)
- Каждое ядро имеет независимый FPU
- Техпроцесс: TSMC 28 нм (ultra-low-power)
- Рабочая температура: −40°C ... +125°C
- Корпус: BGA144 (8 x 8 x 0.953 мм)

### KPU (Neural Network Processor)
- 0.8 TOPS производительность
- Аппаратный ускоритель свёрточных нейросетей
- Поддерживает: object detection, image classification, face recognition, target tracking
- Максимальный размер модели: 5.5M–5.9M

### APU (Audio Processor)
- Обработка микрофонных массивов
- Sound source orientation, sound field imaging, beamforming
- Voice wake-up, speech recognition

### Безопасность
- AES и SHA256 аппаратные ускорители
- Поддержка шифрования прошивки

### Периферия
- DVP, JTAG, OTP, FPIOA, GPIO, UART, SPI, RTC, I2S, I2C, WDT, Timer, PWM
- 48 свободных I/O пинов через FPIOA (255 внутренних функций)
- 32 High-Speed GPIO (GPIOHS) — индивидуальные прерывания
- 8 General-Purpose GPIO — общий источник прерываний
- Двойное напряжение I/O: 3.3V / 1.8V (без уровнего конвертера)

### Даташит
- [K210 Datasheet (PDF)](https://cdn.hackaday.io/files/1654127076987008/kendryte_datasheet_20181011163248_en.pdf)
- [K210 Datasheet (GitHub)](https://github.com/kendryte/kendryte-doc-datasheet)

---

## Сенсор OV7740

| Параметр | Значение |
|---|---|
| Производитель | OmniVision Technologies |
| Технология | OmniPixel3-HS |
| Разрешение | VGA (656 x 496, активных: 656 x 488) |
| FPS | До 60 fps (VGA), до 120 fps (QVGA) |
| Выходные форматы | 8-bit / 10-bit, full-frame / sub-sampled / windowed / scaled |
| Цветовой фильтр | Bayer pattern (BG/GR) |
| Управление | SCCB (Serial Camera Control Bus) |
| Функции | Mirror, Flip, OTP memory |
| FOV в UnitV-M12 | 80° |

### Даташит
- [OV7740 Datasheet (M5Stack OSS)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/stickv/OV7740_datasheet.pdf)
- [OV7740 Datasheet (Alldatasheet)](https://www.alldatasheet.com/datasheet-pdf/pdf/312424/OMNIVISION/OV7740.html)

---

## Программное обеспечение и SDK

### 1. MaixPy (MicroPython для K210)

MaixPy — порт MicroPython для K210. Поддерживает работу с камерой, дисплеем, нейросетями (KPU), аудио, периферией (GPIO, I2C, SPI, UART, PWM, I2S).

**Возможности:**
- Захват и обработка изображений в реальном времени
- Аппаратно-ускоренный AI-инференс (object detection, face recognition, classification)
- Обработка микрофонных массивов
- Wi-Fi (через ESP32/ESP8285, если доступно)
- Управление LED и периферией

**Начало работы:**
1. Установить USB-драйвер (CP2104 / FTDI)
2. Скачать и прошить firmware через **Kflash_GUI** (выбрать board type, порт, файл прошивки, baud rate → Download)
3. Установить **MaixPy IDE** для редактирования, загрузки и выполнения скриптов
4. Подключиться к последовательному порту (115200 bps) через Putty / MobaXterm для REPL

**Ресурсы:**
- [MaixPy Documentation (Sipeed Wiki)](https://wiki.sipeed.com/soft/maixpy/en/)
- [MaixPy-v1 GitHub](https://github.com/sipeed/MaixPy-v1)
- [UnitV MaixPy Quick Start](https://docs.m5stack.com/en/guide/ai_camera/unitv/maixpy)

> **Примечание:** K210-платы постепенно заменяются новыми MaixCAM, но поддержка legacy продолжается.

---

### 2. V-Function (визуальное распознавание)

V-Function — набор прошивок визуального распознавания от M5Stack для UnitV / M5StickV. Камера выполняет распознавание и выводит результат по UART (115200 bps) в формате JSON.

**Поддерживаемые функции:**
| Функция | Описание |
|---|---|
| Motion Detect | Обнаружение движения в кадре |
| Target Trace | Отслеживание целевого объекта |
| Color Trace | Отслеживание по цвету |
| Face Detect | Обнаружение лица |
| QR Code | Распознавание QR-кодов |
| Bar Code | Распознавание штрих-кодов |
| Datamatrix Code | Распознавание Datamatrix |
| Apriltag Code | Распознавание AprilTag маркеров |

**Интеграция:** UIFlow 1.6.2+ (графическое программирование на https://flow.m5stack.com/) — добавить расширение UnitV в панели Unit.

**Ресурсы:**
- [V-Function Guide](https://docs.m5stack.com/en/guide/ai_camera/unitv/v_function)
- [V-Function Quick Start](https://docs.m5stack.com/en/quick_start/unitv/v_function)

---

### 3. V-Training

Сервис обучения пользовательских моделей для V-серии камер.

- [V-Training](https://docs.m5stack.com/en/quick_start/unitv/v_training)

---

### 4. Arduino

- [UnitV-M12 Arduino Tutorial](https://docs.m5stack.com/en/arduino/projects/unit/unitv)
- Данные обмена UnitV ↔ хост: JSON по UART (115200 bps)
- Каждая функция V-Function имеет свой формат JSON

---

### 5. UIFlow (Blockly)

Графическое программирование через блоки в UIFlow.

- [UnitV UIFlow Documentation](https://docs.m5stack.com/en/uiflow/blockly/unit/unitv)

---

## Пример кода: Track Ball с RoverC

MaixPy-скрипт для отслеживания цветного мяча и передачи координат по UART (для управления RoverC).

```python
import sensor
import image
import lcd
import time
import utime
from machine import UART
from Maix import GPIO
from fpioa_manager import *

# Настройка UART на пинах G34(TX), G35(RX)
fm.register(34, fm.fpioa.UART1_TX)
fm.register(35, fm.fpioa.UART1_RX)
uart_out = UART(UART.UART1, 115200, 8, None, 1, timeout=1000, read_buf_len=4096)

# Инициализация камеры
sensor.reset()
sensor.set_pixformat(sensor.RGB565)
sensor.set_framesize(sensor.QVGA)
sensor.run(1)

# LAB-пороги для целевого цвета (настроить под свой объект)
target_lab_threshold = (45, 70, -60, -30, 0, 40)

while True:
    img = sensor.snapshot()

    blobs = img.find_blobs(
        [target_lab_threshold],
        x_stride=2, y_stride=2,
        pixels_threshold=100,
        merge=True, margin=20
    )

    if blobs:
        # Найти самый большой blob
        max_area = 0
        target = blobs[0]
        for b in blobs:
            if b.area() > max_area:
                max_area = b.area()
                target = b

        if uart_out.read(4096):
            area = target.area()
            dx = 120 - target[6]  # Отклонение от центра
            hexlist = [
                (dx >> 8) & 0xFF, dx & 0xFF,
                (area >> 16) & 0xFF, (area >> 8) & 0xFF, area & 0xFF
            ]
            uart_out.write(bytes(hexlist))

        # Визуализация на LCD
        img.draw_rectangle(target[0:4])
        img.draw_cross(target[5], target[6])
    else:
        if uart_out.read(4096):
            hexlist = [0x80, 0x00, 0x00, 0x00, 0x00]
            uart_out.write(bytes(hexlist))
```

**Протокол UART (5 байт):**
- Байты 0-1: dx (отклонение по X от центра, signed 16-bit)
- Байты 2-4: area (площадь blob, unsigned 24-bit)
- Если объект не найден: `[0x80, 0x00, 0x00, 0x00, 0x00]`

**Источник:** [M5-ProductExampleCodes/App/UnitV/track_ball](https://github.com/m5stack/M5-ProductExampleCodes/tree/master/App/UnitV/track_ball)

---

## Размерный чертёж

```
        ┌─────────────┐
        │   12.7       │ 16.4
        │  ┌───────┐   │
   40.0 │  │  M5   │   │
        │  │  ⊙lens│   │
        │  │  Ø14   │   │
        │  └───────┘   │
        └──┤USB-C├─────┘
           └──12──┘
              24.0

Lens diameter: Ø14 mm
Lens protrusion: Ø4.65 mm
Corner radius: R3, R0.5
Connector width: 12 mm, height: 5.2 mm
Side depth: 11.8 mm, bottom offset: 16 mm
UNIT: mm
```

---

## Все ссылки и ресурсы

### Документация продукта
- [UnitV-M12 — M5Stack Docs](https://docs.m5stack.com/en/unit/UNIT-V%20M12)
- [UnitV-M12 PDF Datasheet](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/static/pdf/static/en/unit/UNIT-V%20M12.pdf)
- [M5Stack Shop — UnitV-M12](https://shop.m5stack.com/products/unitv-k210-ai-camera-m12-version-ov7740)

### Даташиты компонентов
- [Kendryte K210 Datasheet (PDF)](https://cdn.hackaday.io/files/1654127076987008/kendryte_datasheet_20181011163248_en.pdf)
- [Kendryte K210 Datasheet (GitHub)](https://github.com/kendryte/kendryte-doc-datasheet)
- [OV7740 Datasheet (M5Stack)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/datasheet/core/stickv/OV7740_datasheet.pdf)

### SDK и среды разработки
- [MaixPy Documentation](https://wiki.sipeed.com/soft/maixpy/en/)
- [MaixPy-v1 GitHub](https://github.com/sipeed/MaixPy-v1)
- [UnitV MaixPy Getting Started](https://docs.m5stack.com/en/guide/ai_camera/unitv/maixpy)

### Прошивки визуального распознавания
- [V-Function Guide](https://docs.m5stack.com/en/guide/ai_camera/unitv/v_function)
- [V-Training](https://docs.m5stack.com/en/quick_start/unitv/v_training)

### Туториалы и примеры
- [Arduino Tutorial](https://docs.m5stack.com/en/arduino/projects/unit/unitv)
- [UIFlow (Blockly)](https://docs.m5stack.com/en/uiflow/blockly/unit/unitv)
- [Track Ball Example (GitHub)](https://github.com/m5stack/M5-ProductExampleCodes/tree/master/App/UnitV/track_ball)

### Видео
- Color Recognition Example — unitV.mp4

### Сравнение продуктов
- [Product Selection Table (UnitV series)](https://docs.m5stack.com/en/unit)
