/*
 * ssd1306.c
 * Framebuffer-based SSD1306 driver. Text drawn into `buffer`, then
 * SSD1306_UpdateScreen() pushes the whole 1KB framebuffer over I2C.
 */

#include "ssd1306.h"
#include "ssd1306_font5x7.h"
#include <string.h>

#define SSD1306_CMD   0x00
#define SSD1306_DATA  0x40

static HAL_StatusTypeDef write_cmd(SSD1306_HandleTypeDef *dev, uint8_t cmd)
{
    uint8_t pkt[2] = { SSD1306_CMD, cmd };
    return HAL_I2C_Master_Transmit(dev->hi2c, dev->dev_addr, pkt, 2, 100);
}

HAL_StatusTypeDef SSD1306_Init(SSD1306_HandleTypeDef *dev, I2C_HandleTypeDef *hi2c, uint16_t addr)
{
    dev->hi2c = hi2c;
    dev->dev_addr = addr;
    memset(dev->buffer, 0, sizeof(dev->buffer));

    HAL_Delay(100); /* let the panel power rail settle */

    const uint8_t init_seq[] = {
        0xAE,       /* display off */
        0xD5, 0x80, /* clock divide */
        0xA8, 0x3F, /* multiplex ratio 64 */
        0xD3, 0x00, /* display offset = 0 */
        0x40,       /* start line = 0 */
        0x8D, 0x14, /* charge pump enable */
        0x20, 0x00, /* memory addressing mode = horizontal */
        0xA1,       /* segment remap */
        0xC8,       /* COM scan direction remapped */
        0xDA, 0x12, /* COM pins config for 128x64 */
        0x81, 0x7F, /* contrast */
        0xD9, 0xF1, /* pre-charge period */
        0xDB, 0x40, /* VCOMH deselect level */
        0xA4,       /* resume to RAM content */
        0xA6,       /* normal (not inverted) display */
        0xAF        /* display on */
    };

    for (uint32_t i = 0; i < sizeof(init_seq); i++) {
        if (write_cmd(dev, init_seq[i]) != HAL_OK) return HAL_ERROR;
    }
    return HAL_OK;
}

void SSD1306_Clear(SSD1306_HandleTypeDef *dev)
{
    memset(dev->buffer, 0, sizeof(dev->buffer));
}

void SSD1306_DrawChar(SSD1306_HandleTypeDef *dev, uint8_t x, uint8_t page, char c)
{
    if (c < 32 || c > 95) c = 32;
    const uint8_t *glyph = font5x7[c - 32];
    if (page >= SSD1306_PAGES) return;

    for (uint8_t col = 0; col < 5; col++) {
        uint8_t px = x + col;
        if (px >= SSD1306_WIDTH) return;
        dev->buffer[page * SSD1306_WIDTH + px] = glyph[col];
    }
    /* one blank column of spacing after each glyph */
    uint8_t spacer = x + 5;
    if (spacer < SSD1306_WIDTH) dev->buffer[page * SSD1306_WIDTH + spacer] = 0x00;
}

void SSD1306_DrawString(SSD1306_HandleTypeDef *dev, uint8_t x, uint8_t page, const char *str)
{
    uint8_t cursor = x;
    while (*str && cursor < SSD1306_WIDTH) {
        char c = *str++;
        if (c >= 'a' && c <= 'z') c -= 32; /* fold lowercase to uppercase, font only covers 32-95 */
        SSD1306_DrawChar(dev, cursor, page, c);
        cursor += 6; /* 5px glyph + 1px spacing */
    }
}

HAL_StatusTypeDef SSD1306_UpdateScreen(SSD1306_HandleTypeDef *dev)
{
    for (uint8_t page = 0; page < SSD1306_PAGES; page++) {
        write_cmd(dev, 0xB0 + page); /* set page address */
        write_cmd(dev, 0x00);        /* lower column = 0 */
        write_cmd(dev, 0x10);        /* higher column = 0 */

        uint8_t pkt[SSD1306_WIDTH + 1];
        pkt[0] = SSD1306_DATA;
        memcpy(&pkt[1], &dev->buffer[page * SSD1306_WIDTH], SSD1306_WIDTH);
        if (HAL_I2C_Master_Transmit(dev->hi2c, dev->dev_addr, pkt, sizeof(pkt), 100) != HAL_OK)
            return HAL_ERROR;
    }
    return HAL_OK;
}
