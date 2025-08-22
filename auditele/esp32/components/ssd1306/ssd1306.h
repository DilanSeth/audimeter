/*
 * Driver simplificado para display SSD1306 OLED
 * Compatible con I2C para ESP32
 */

#ifndef SSD1306_H
#define SSD1306_H

#include "driver/i2c.h"
#include "esp_log.h"

#define SSD1306_I2C_ADDR    0x3C
#define SSD1306_I2C_PORT    I2C_NUM_0
#define SSD1306_I2C_FREQ    400000

typedef struct {
    i2c_port_t i2c_port;
    uint8_t width;
    uint8_t height;
    uint8_t pages;
} SSD1306_t;

// Funciones principales
esp_err_t i2c_master_init(SSD1306_t* dev, int sda, int scl, int reset);
void ssd1306_init(SSD1306_t* dev, int width, int height);
void ssd1306_clear_screen(SSD1306_t* dev, bool invert);
void ssd1306_contrast(SSD1306_t* dev, int contrast);
void ssd1306_display_text(SSD1306_t* dev, int page, char* text, int text_len, bool invert);

// Comandos SSD1306
#define SSD1306_CONTROL_CMD_STREAM    0x00
#define SSD1306_CONTROL_DATA_STREAM   0x40

// Implementación básica inline para reducir dependencias
static inline esp_err_t ssd1306_write_command(SSD1306_t* dev, uint8_t command) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SSD1306_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, SSD1306_CONTROL_CMD_STREAM, true);
    i2c_master_write_byte(cmd, command, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(dev->i2c_port, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

static inline esp_err_t ssd1306_write_data(SSD1306_t* dev, uint8_t* data, size_t data_len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SSD1306_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, SSD1306_CONTROL_DATA_STREAM, true);
    i2c_master_write(cmd, data, data_len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(dev->i2c_port, cmd, 1000 / portTICK_RATE_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

// Font básico 8x8
extern const uint8_t font8x8_basic[128][8];

#endif // SSD1306_H