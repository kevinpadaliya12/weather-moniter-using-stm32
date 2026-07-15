# weather-moniter-using-stm32
# RTOS-Based Weather Station

Multi-tasking environmental monitor built on an STM32 (Cortex-M) MCU
running FreeRTOS. Reads temperature, humidity, and pressure from a BME280,
polls a gas sensor (MQ-135) over ADC, and drives an SSD1306 OLED — all as
independent, non-blocking tasks instead of one big polling loop.

**Status:** RTOS task/queue/mutex architecture verified over serial on a
bare Nucleo-F446RE (no sensor hardware attached yet). Sensor and OLED
drivers are written and ready to wire in once hardware arrives.

## Why this exists

Most "sensor + display" tutorials use a single `while(1)` loop that reads
a sensor, then draws to a screen, then delays — fine until one of those
steps blocks longer than expected and everything else stalls with it.
This project separates concerns into FreeRTOS tasks so a slow I2C
transaction or a busy display refresh can't stall sensor sampling, and
vice versa.

              
   

- **SensorTask** owns all sampling — BME280 over I2C, MQ-135 over ADC —
  and pushes the latest reading onto a length-1 queue every 2 seconds.
- **DisplayTask** blocks on that queue and redraws the OLED only when
  fresh data arrives, rather than polling on a fixed timer of its own.
- A **mutex** guards the shared I2C1 bus so the two tasks can never
  interleave transactions and corrupt each other's frames.

## Hardware

| Component        | Interface     | Notes                                  |
|-------------------|--------------|------------------------------------------|
| STM32 Nucleo-F446RE (or similar F4/F1) | —             | any Cortex-M board with FreeRTOS + HAL support |
| BME280            | I2C1 (shared) | temperature / humidity / pressure       |
| SSD1306 128x64     | I2C1 (shared) | OLED status display                     |
| MQ-135             | ADC1          | gas / air-quality sensor, raw ADC counts |



## Getting started

1. Generate a CubeMX project for your board with I2C1, ADC1, and
   FreeRTOS (CMSIS_V2) 
2. Drop the files from `Core/Inc` and `Core/Src` into your generated
   project.
3. No sensors yet? Build with `app_freertos_mock.c` and verify the task
   pipeline over serial
4. Sensors in hand? Swap to `app_freertos.c`, wire up BME280 + SSD1306 +
   MQ-135 per the wiring table, and flash.

## Roadmap

- [x] FreeRTOS task/queue/mutex skeleton, verified over UART with
      simulated sensor data
- [x] BME280 driver (I2C, full calibration + compensation)
- [x] SSD1306 driver (framebuffer + basic text rendering)
- [ ] Hardware validation with real BME280 / SSD1306 / MQ-135
- [ ] MQ-135 ppm calibration (Rs/Ro curve, clean-air baseline)
- [ ] Optional: push readings over UART/BLE for logging

## License

MIT — see `LICENSE` (add one if you haven't yet).
