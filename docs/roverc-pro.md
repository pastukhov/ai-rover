# RoverC-Pro

**SKU:** K036-B
**Источник:** [docs.m5stack.com](https://docs.m5stack.com/en/hat/hat_roverc_pro)

## Описание

RoverC-Pro — программируемая омнинаправленная мобильная робототехническая платформа на колёсах Mecanum. Совместима с M5StickC / M5StickC PLUS — достаточно просто вставить контроллер в слот.

Основной управляющий чип — **STM32F030C6T6**. Платформа оснащена четырьмя мотор-редукторами N20 (worm gear), управляемыми драйвером **L9110S**.

PRO-версия включает **захватный механизм** (gripper), управляемый сервоприводом. На плате предусмотрено **2 интерфейса для серво**.

Дополнительно имеются **2 Grove-совместимых I2C-разъёма** для подключения внешних модулей. Платформа совместима с **отверстиями LEGO** для конструктивного расширения.

На задней стороне расположена **съёмная перезаряжаемая батарея 16340 (700 мАч)**. Зарядка батареи осуществляется через M5StickC / M5StickC Plus. Также сзади расположены выключатель питания и индикаторный светодиод.

## Особенности (Features)

- I2C-адрес: **0x38**
- Дистанционное управление
- Захватная конструкция (gripper)
- Программируемость
- Омнинаправленное движение (Mecanum wheels)
- 4-канальный драйвер моторов
- Совместимость с LEGO
- Дополнительные Grove-интерфейсы для расширения
- Батарея 16340 (700 мАч)

## Комплект поставки (Includes)

- 1 x RoverC-Pro
- 1 x Gripper Kit (набор захвата)

## Применения (Applications)

- Мини-разведывательный автомобиль
- Малый мобильный робот
- Умные игрушки

## Спецификации

| Параметр | Значение |
|---|---|
| MCU | STM32F030C8T6 |
| Протокол | I2C: 0x38 |
| Размер продукта | 120.0 x 75.0 x 58.0 мм |
| Вес продукта | 169.3 г |
| Размер упаковки | 115.0 x 85.0 x 65.0 мм |
| Вес брутто | 245.0 г |

## Распиновка (PinMap)

| Компонент | Pin 1 | Pin 2 | Pin 3 | Pin 4 |
|---|---|---|---|---|
| M5StickC | G26 | G0 | 5V | GND |
| RoverC HAT | SCL | SDA | 5V | GND |
| I2C① (Grove) | SCL | SDA | 5V | GND |
| I2C② (Grove) | SCL | SDA | 5V | GND |

## Протокол I2C

- **Тип связи:** I2C
- **I2C-адрес:** `0x38`
- **Рекомендуемая скорость:** 100–400 кГц
- [Документ протокола I2C (PDF)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/hat/hat_roverc_pro/K036-B_I2C_PROTOCOL_CN.pdf)

### 2.1 Motor Speed — регистр `0x00`

Управление скоростью 4 моторов. Каждый мотор управляется 1 байтом (signed).

| Адрес | R/W | Размер | Параметр |
|---|---|---|---|
| `0x00` | R/W | 1 byte | Motor1 Speed (-127 ~ 127) |
| `0x01` | R/W | 1 byte | Motor2 Speed (-127 ~ 127) |
| `0x02` | R/W | 1 byte | Motor3 Speed (-127 ~ 127) |
| `0x03` | R/W | 1 byte | Motor4 Speed (-127 ~ 127) |

- **Значение:** `int8_t`, диапазон от -127 до +127
- **Положительное** — вращение в одну сторону, **отрицательное** — в обратную
- **0** — остановка мотора

### 2.2 Servo Angle Control — регистр `0x10`

Управление углом поворота сервоприводов (по значению угла).

| Адрес | R/W | Размер | Параметр |
|---|---|---|---|
| `0x10` | R/W | 1 byte | Servo1 Angle (0 ~ 180) |
| `0x11` | R/W | 1 byte | Servo2 Angle (0 ~ 180) |

- **Значение:** `uint8_t`, диапазон 0–180
- Соответствует углу поворота серво от 0° до 180°

### 2.3 Servo Pulse Control — регистр `0x20`

Управление сервоприводами через ширину импульса (pulse width). Каждый серво использует 2 байта (High Byte + Low Byte).

| Адрес | R/W | Размер | Параметр |
|---|---|---|---|
| `0x20` | R/W | 1 byte | Servo1 Pulse High Byte |
| `0x21` | R/W | 1 byte | Servo1 Pulse Low Byte |
| `0x22` | R/W | 1 byte | Servo2 Pulse High Byte |
| `0x23` | R/W | 1 byte | Servo2 Pulse Low Byte |

- **Значение:** `uint16_t` (2 байта, big-endian), диапазон 500–2500
- Частота управления серво по умолчанию: **50 Гц**
- Ширина импульса 500–2500 мкс соответствует углу поворота **0°–180°**

## Программное обеспечение

### Arduino

**Пример 1 — Беспроводное управление (RoverC + JoyC через UDP):**

- [RoverC-Pro & JoyC Remote Control — M5StickC](https://github.com/m5stack/M5-RoverC/tree/master/examples/RoverC_M5StickC/JoyC_%26_RoverC)
- [RoverC-Pro & JoyC Remote Control — M5StickC-Plus](https://github.com/m5stack/M5-RoverC/tree/master/examples/RoverC_M5StickCPlus/JoyC_%26_RoverC)

> После включения RoverC создаёт точку доступа «M5AP + 2-байтный MAC-адрес», а JoyC подключается к ней автоматически.

**Пример 2 — Автономное управление (прямое управление с основного контроллера):**

- [RoverC-Pro Example — M5StickC](https://github.com/m5stack/M5-RoverC/blob/master/examples/RoverC_M5StickC/RunningRoverC/RunningRoverC.ino)
- [RoverC-Pro Example — M5StickC-Plus](https://github.com/m5stack/M5-RoverC/blob/master/examples/RoverC_M5StickCPlus/RunningRoverC/RunningRoverC.ino)

### UiFlow1

- [RoverC-Pro UiFlow1 Docs](https://docs.m5stack.com/en/uiflow/blockly/hat/roverc)

### EasyLoader

- [Скачать EasyLoader RoverC-Pro (Windows)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/EasyLoader/Windows/HAT/EasyLoader_RoverC_PRO_Alone.exe)

## Сравнение RoverC-Pro и RoverC

| Характеристика | RoverC PRO | RoverC |
|---|---|---|
| Серво-захват (Gripper) | x1 | — |
| Интерфейсы расширения серво | x2 | — |
| Батарея | Съёмная | Несъёмная |

## Совместимые контроллеры

- StickS3
- Arduino Nesso N1
- StickC-Plus2
- StickC-Plus
- StickC
- CoreInk

## Ресурсы

- [Документация продукта (PDF)](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/static/pdf/static/en/hat/hat_roverc_pro.pdf)
- [3D-модели / Structure Files](https://github.com/m5stack/M5_Hardware/tree/master/Products/K036-B_RoverC-Pro/Structures)
- [Купить на M5Stack Store](https://shop.m5stack.com/products/roverc-prow-o-m5stickc)
- [GitHub: M5-RoverC](https://github.com/m5stack/M5-RoverC)
