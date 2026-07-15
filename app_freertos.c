/*
 * app_freertos.c
 *
 * Application-level FreeRTOS wiring. CubeMX generates freertos.c with
 * MX_FREERTOS_Init() — call App_FreeRTOS_Init() from inside that function
 * (or directly before osKernelStart() in main.c) instead of editing the
 * generated file directly, so regenerating from CubeMX won't wipe this out.
 *
 * Assumes CubeMX config:
 *   - I2C1 -> BME280 + SSD1306 (both on same bus)
 *   - ADC1 channel -> MQ135 analog pin
 *   - FreeRTOS middleware, CMSIS_V2 interface
 */

#include "cmsis_os2.h"
#include "bme280.h"
#include "ssd1306.h"
#include <stdio.h>

extern I2C_HandleTypeDef  hi2c1;   /* from CubeMX-generated main.c */
extern ADC_HandleTypeDef  hadc1;   /* from CubeMX-generated main.c */

/* ---- shared state ---- */
typedef struct {
    float temperature;
    float humidity;
    float pressure;
    uint16_t gas_raw;
} SensorData_t;

static BME280_HandleTypeDef bme280;
static SSD1306_HandleTypeDef oled;

static osMutexId_t   i2cMutexHandle;
static osMessageQueueId_t sensorQueueHandle;

static osThreadId_t sensorTaskHandle;
static osThreadId_t displayTaskHandle;

/* ---- MQ135: single-shot ADC polling read ---- */
static uint16_t MQ135_ReadRaw(void)
{
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 50) == HAL_OK) {
        uint16_t val = (uint16_t)HAL_ADC_GetValue(&hadc1);
        HAL_ADC_Stop(&hadc1);
        return val;
    }
    HAL_ADC_Stop(&hadc1);
    return 0;
}

/* ---- Task: reads BME280 + MQ135 every 2s, pushes to queue ---- */
static void StartSensorTask(void *argument)
{
    (void)argument;
    SensorData_t data;

    for (;;) {
        osMutexAcquire(i2cMutexHandle, osWaitForever);

        BME280_Data env;
        HAL_StatusTypeDef status = BME280_ReadAll(&bme280, &env);

        osMutexRelease(i2cMutexHandle);

        /* ADC read does not need the I2C mutex, it's a different peripheral */
        uint16_t gas = MQ135_ReadRaw();

        if (status == HAL_OK) {
            data.temperature = env.temperature;
            data.humidity    = env.humidity;
            data.pressure    = env.pressure;
            data.gas_raw     = gas;

            /* non-blocking put: if display task is behind, drop the oldest
             * reading rather than stalling the sensor task */
            osMessageQueueReset(sensorQueueHandle);
            osMessageQueuePut(sensorQueueHandle, &data, 0, 0);
        }

        osDelay(2000);
    }
}

/* ---- Task: waits for fresh sensor data, redraws OLED ---- */
static void StartDisplayTask(void *argument)
{
    (void)argument;
    SensorData_t data;
    char line[24];

    for (;;) {
        if (osMessageQueueGet(sensorQueueHandle, &data, NULL, osWaitForever) == osOK) {

            osMutexAcquire(i2cMutexHandle, osWaitForever);

            SSD1306_Clear(&oled);

            snprintf(line, sizeof(line), "TEMP  %.1f C", data.temperature);
            SSD1306_DrawString(&oled, 0, 0, line);

            snprintf(line, sizeof(line), "HUMID %.1f %%", data.humidity);
            SSD1306_DrawString(&oled, 0, 2, line);

            snprintf(line, sizeof(line), "PRESS %.0f HPA", data.pressure);
            SSD1306_DrawString(&oled, 0, 4, line);

            snprintf(line, sizeof(line), "GAS   %u", data.gas_raw);
            SSD1306_DrawString(&oled, 0, 6, line);

            SSD1306_UpdateScreen(&oled);

            osMutexRelease(i2cMutexHandle);
        }
    }
}

/* ---- Public entry point: call this once before osKernelStart() ---- */
void App_FreeRTOS_Init(void)
{
    /* mutex protecting the shared I2C1 bus between BME280 and SSD1306 */
    const osMutexAttr_t i2c_mutex_attr = { .name = "i2cMutex" };
    i2cMutexHandle = osMutexNew(&i2c_mutex_attr);

    /* queue holds exactly one "latest" reading */
    sensorQueueHandle = osMessageQueueNew(1, sizeof(SensorData_t), NULL);

    /* init peripherals BEFORE the scheduler starts, so no task/mutex race */
    if (BME280_Init(&bme280, &hi2c1, BME280_I2C_ADDR_PRIMARY) != HAL_OK) {
        /* Try secondary address (SDO pulled high) as fallback */
        BME280_Init(&bme280, &hi2c1, BME280_I2C_ADDR_SECONDARY);
    }
    SSD1306_Init(&oled, &hi2c1, SSD1306_I2C_ADDR);

    const osThreadAttr_t sensor_attr = {
        .name = "SensorTask",
        .priority = osPriorityAboveNormal,
        .stack_size = 512
    };
    sensorTaskHandle = osThreadNew(StartSensorTask, NULL, &sensor_attr);

    const osThreadAttr_t display_attr = {
        .name = "DisplayTask",
        .priority = osPriorityNormal,
        .stack_size = 1024   /* snprintf with floats needs more stack */
    };
    displayTaskHandle = osThreadNew(StartDisplayTask, NULL, &display_attr);
}
