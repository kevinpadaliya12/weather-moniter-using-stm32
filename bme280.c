/*
 * bme280.c
 *
 * Register map + compensation formulas per Bosch BME280 datasheet.
 * Floating point compensation variant used for readability;
 * Cortex-M4/M7 with FPU handles this with negligible cost.
 */

#include "bme280.h"
#include <string.h>

/* ---- Register addresses ---- */
#define REG_CALIB_00    0x88
#define REG_CALIB_25    0xA1
#define REG_CALIB_26    0xE1
#define REG_CHIP_ID     0xD0
#define REG_RESET       0xE0
#define REG_CTRL_HUM    0xF2
#define REG_STATUS      0xF3
#define REG_CTRL_MEAS   0xF4
#define REG_CONFIG      0xF5
#define REG_PRESS_MSB   0xF7  /* pressure(3) + temp(3) + hum(2) = 8 bytes burst read */

static HAL_StatusTypeDef read_regs(BME280_HandleTypeDef *dev, uint8_t reg, uint8_t *buf, uint16_t len)
{
    return HAL_I2C_Mem_Read(dev->hi2c, dev->dev_addr, reg, I2C_MEMADD_SIZE_8BIT, buf, len, 100);
}

static HAL_StatusTypeDef write_reg(BME280_HandleTypeDef *dev, uint8_t reg, uint8_t val)
{
    return HAL_I2C_Mem_Write(dev->hi2c, dev->dev_addr, reg, I2C_MEMADD_SIZE_8BIT, &val, 1, 100);
}

HAL_StatusTypeDef BME280_Init(BME280_HandleTypeDef *dev, I2C_HandleTypeDef *hi2c, uint16_t addr)
{
    memset(dev, 0, sizeof(*dev));
    dev->hi2c = hi2c;
    dev->dev_addr = addr;

    uint8_t chip_id = 0;
    if (read_regs(dev, REG_CHIP_ID, &chip_id, 1) != HAL_OK) return HAL_ERROR;
    if (chip_id != 0x60) return HAL_ERROR; /* not a BME280 */

    /* soft reset */
    write_reg(dev, REG_RESET, 0xB6);
    HAL_Delay(10);

    uint8_t c1[26], c2[7];
    if (read_regs(dev, REG_CALIB_00, c1, 26) != HAL_OK) return HAL_ERROR;
    if (read_regs(dev, REG_CALIB_26, c2, 7) != HAL_OK) return HAL_ERROR;

    dev->dig_T1 = (uint16_t)(c1[1] << 8 | c1[0]);
    dev->dig_T2 = (int16_t)(c1[3] << 8 | c1[2]);
    dev->dig_T3 = (int16_t)(c1[5] << 8 | c1[4]);

    dev->dig_P1 = (uint16_t)(c1[7] << 8 | c1[6]);
    dev->dig_P2 = (int16_t)(c1[9] << 8 | c1[8]);
    dev->dig_P3 = (int16_t)(c1[11] << 8 | c1[10]);
    dev->dig_P4 = (int16_t)(c1[13] << 8 | c1[12]);
    dev->dig_P5 = (int16_t)(c1[15] << 8 | c1[14]);
    dev->dig_P6 = (int16_t)(c1[17] << 8 | c1[16]);
    dev->dig_P7 = (int16_t)(c1[19] << 8 | c1[18]);
    dev->dig_P8 = (int16_t)(c1[21] << 8 | c1[20]);
    dev->dig_P9 = (int16_t)(c1[23] << 8 | c1[22]);

    dev->dig_H1 = c1[25];
    dev->dig_H2 = (int16_t)(c2[1] << 8 | c2[0]);
    dev->dig_H3 = c2[2];
    dev->dig_H4 = (int16_t)((c2[3] << 4) | (c2[4] & 0x0F));
    dev->dig_H5 = (int16_t)((c2[5] << 4) | (c2[4] >> 4));
    dev->dig_H6 = (int8_t)c2[6];

    /* humidity oversampling x1 */
    if (write_reg(dev, REG_CTRL_HUM, 0x01) != HAL_OK) return HAL_ERROR;
    /* temp oversampling x1, press oversampling x1, normal mode */
    if (write_reg(dev, REG_CTRL_MEAS, (0x01 << 5) | (0x01 << 2) | 0x03) != HAL_OK) return HAL_ERROR;
    /* standby 1000ms, filter off */
    if (write_reg(dev, REG_CONFIG, (0x05 << 5)) != HAL_OK) return HAL_ERROR;

    return HAL_OK;
}

static float compensate_temperature(BME280_HandleTypeDef *dev, int32_t adc_T)
{
    float var1, var2, T;
    var1 = (((float)adc_T) / 16384.0f - ((float)dev->dig_T1) / 1024.0f) * ((float)dev->dig_T2);
    var2 = ((((float)adc_T) / 131072.0f - ((float)dev->dig_T1) / 8192.0f) *
            (((float)adc_T) / 131072.0f - ((float)dev->dig_T1) / 8192.0f)) * ((float)dev->dig_T3);
    dev->t_fine = (int32_t)(var1 + var2);
    T = (var1 + var2) / 5120.0f;
    return T;
}

static float compensate_pressure(BME280_HandleTypeDef *dev, int32_t adc_P)
{
    float var1, var2, p;
    var1 = ((float)dev->t_fine / 2.0f) - 64000.0f;
    var2 = var1 * var1 * ((float)dev->dig_P6) / 32768.0f;
    var2 = var2 + var1 * ((float)dev->dig_P5) * 2.0f;
    var2 = (var2 / 4.0f) + (((float)dev->dig_P4) * 65536.0f);
    var1 = (((float)dev->dig_P3) * var1 * var1 / 524288.0f + ((float)dev->dig_P2) * var1) / 524288.0f;
    var1 = (1.0f + var1 / 32768.0f) * ((float)dev->dig_P1);
    if (var1 == 0.0f) return 0; /* avoid div by zero */
    p = 1048576.0f - (float)adc_P;
    p = (p - (var2 / 4096.0f)) * 6250.0f / var1;
    var1 = ((float)dev->dig_P9) * p * p / 2147483648.0f;
    var2 = p * ((float)dev->dig_P8) / 32768.0f;
    p = p + (var1 + var2 + ((float)dev->dig_P7)) / 16.0f;
    return p / 100.0f; /* Pa -> hPa */
}

static float compensate_humidity(BME280_HandleTypeDef *dev, int32_t adc_H)
{
    float var_H;
    var_H = (((float)dev->t_fine) - 76800.0f);
    var_H = (adc_H - (((float)dev->dig_H4) * 64.0f + ((float)dev->dig_H5) / 16384.0f * var_H)) *
            (((float)dev->dig_H2) / 65536.0f * (1.0f + ((float)dev->dig_H6) / 67108864.0f * var_H *
            (1.0f + ((float)dev->dig_H3) / 67108864.0f * var_H)));
    var_H = var_H * (1.0f - ((float)dev->dig_H1) * var_H / 524288.0f);
    if (var_H > 100.0f) var_H = 100.0f;
    if (var_H < 0.0f) var_H = 0.0f;
    return var_H;
}

HAL_StatusTypeDef BME280_ReadAll(BME280_HandleTypeDef *dev, BME280_Data *out)
{
    uint8_t buf[8];
    if (read_regs(dev, REG_PRESS_MSB, buf, 8) != HAL_OK) return HAL_ERROR;

    int32_t adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
    int32_t adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);
    int32_t adc_H = ((int32_t)buf[6] << 8) | buf[7];

    out->temperature = compensate_temperature(dev, adc_T); /* must run first: sets t_fine */
    out->pressure    = compensate_pressure(dev, adc_P);
    out->humidity    = compensate_humidity(dev, adc_H);

    return HAL_OK;
}
