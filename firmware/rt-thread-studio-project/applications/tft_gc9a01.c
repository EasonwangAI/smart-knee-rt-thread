#include <rtthread.h>
#include <drivers/pin.h>
#ifdef FINSH_USING_MSH
#include <finsh.h>
#endif

#include "tft_gc9a01.h"

#define LCD_CMD_CASET                    0x2A
#define LCD_CMD_RASET                    0x2B
#define LCD_CMD_RAMWR                    0x2C

static rt_bool_t tft_ready = RT_FALSE;

static void tft_delay_ms(rt_uint32_t ms)
{
    rt_thread_mdelay(ms);
}

static void tft_gpio_write(rt_base_t pin, rt_base_t level)
{
    rt_pin_write(pin, level);
}

static void tft_spi_write8(uint8_t data)
{
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        tft_gpio_write(TFT_GC9A01_PIN_SCL, PIN_LOW);
        tft_gpio_write(TFT_GC9A01_PIN_SDA, (data & 0x80) ? PIN_HIGH : PIN_LOW);
        tft_gpio_write(TFT_GC9A01_PIN_SCL, PIN_HIGH);
        data <<= 1;
    }
}

static void tft_write_cmd(uint8_t cmd)
{
    tft_gpio_write(TFT_GC9A01_PIN_DC, PIN_LOW);
    tft_gpio_write(TFT_GC9A01_PIN_CS, PIN_LOW);
    tft_spi_write8(cmd);
    tft_gpio_write(TFT_GC9A01_PIN_CS, PIN_HIGH);
}

static void tft_write_data(const uint8_t *data, uint8_t len)
{
    uint8_t i;

    tft_gpio_write(TFT_GC9A01_PIN_DC, PIN_HIGH);
    tft_gpio_write(TFT_GC9A01_PIN_CS, PIN_LOW);
    for (i = 0; i < len; i++)
    {
        tft_spi_write8(data[i]);
    }
    tft_gpio_write(TFT_GC9A01_PIN_CS, PIN_HIGH);
}

static void tft_cmd_data(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    tft_write_cmd(cmd);
    if (len > 0)
    {
        tft_write_data(data, len);
    }
}

static void tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4];

    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)x0;
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)x1;
    tft_cmd_data(LCD_CMD_CASET, data, 4);

    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)y0;
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)y1;
    tft_cmd_data(LCD_CMD_RASET, data, 4);

    tft_write_cmd(LCD_CMD_RAMWR);
}

static uint8_t ascii5x7(char c, uint8_t col)
{
    const uint8_t *p = RT_NULL;
    static const uint8_t sp[5] = {0, 0, 0, 0, 0};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t pct[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
    static const uint8_t slash[5] = {0x40, 0x30, 0x08, 0x06, 0x01};
    static const uint8_t digits[10][5] = {
        {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
        {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
        {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    };
    static const uint8_t upper[26][5] = {
        {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
        {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
        {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
        {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
        {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
        {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
        {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
        {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
        {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
        {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    };

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9') p = digits[c - '0'];
    else if (c >= 'A' && c <= 'Z') p = upper[c - 'A'];
    else if (c == ':') p = colon;
    else if (c == '-') p = dash;
    else if (c == '%') p = pct;
    else if (c == '/') p = slash;
    else p = sp;

    return p[col];
}

void tft_gc9a01_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    uint32_t count;

    if (!tft_ready) return;
    if (x >= TFT_GC9A01_WIDTH || y >= TFT_GC9A01_HEIGHT) return;
    if ((uint32_t)x + w > TFT_GC9A01_WIDTH)  w = TFT_GC9A01_WIDTH - x;
    if ((uint32_t)y + h > TFT_GC9A01_HEIGHT) h = TFT_GC9A01_HEIGHT - y;
    if (w == 0 || h == 0) return;

    tft_set_window(x, y, x + w - 1, y + h - 1);
    tft_gpio_write(TFT_GC9A01_PIN_DC, PIN_HIGH);
    tft_gpio_write(TFT_GC9A01_PIN_CS, PIN_LOW);
    for (count = (uint32_t)w * h; count > 0; count--)
    {
        tft_spi_write8((uint8_t)(color >> 8));
        tft_spi_write8((uint8_t)color);
    }
    tft_gpio_write(TFT_GC9A01_PIN_CS, PIN_HIGH);
}

void tft_gc9a01_fill(uint16_t color)
{
    tft_gc9a01_fill_rect(0, 0, TFT_GC9A01_WIDTH, TFT_GC9A01_HEIGHT, color);
}

static void tft_draw_char(uint16_t x, uint16_t y, char c,
                          uint16_t color, uint16_t bg, uint8_t scale)
{
    uint16_t dx, dy;
    uint16_t w = (uint16_t)(6 * scale);
    uint16_t h = (uint16_t)(8 * scale);

    if (x >= TFT_GC9A01_WIDTH || y >= TFT_GC9A01_HEIGHT) return;
    if ((uint32_t)x + w > TFT_GC9A01_WIDTH) return;
    if ((uint32_t)y + h > TFT_GC9A01_HEIGHT) return;

    tft_set_window(x, y, x + w - 1, y + h - 1);
    tft_gpio_write(TFT_GC9A01_PIN_DC, PIN_HIGH);
    tft_gpio_write(TFT_GC9A01_PIN_CS, PIN_LOW);
    for (dy = 0; dy < h; dy++)
    {
        uint8_t row = (uint8_t)(dy / scale);
        for (dx = 0; dx < w; dx++)
        {
            uint8_t col = (uint8_t)(dx / scale);
            uint8_t bits = (col < 5) ? ascii5x7(c, col) : 0;
            uint16_t pix = (bits & (1 << row)) ? color : bg;
            tft_spi_write8((uint8_t)(pix >> 8));
            tft_spi_write8((uint8_t)pix);
        }
    }
    tft_gpio_write(TFT_GC9A01_PIN_CS, PIN_HIGH);
}

void tft_gc9a01_draw_text(uint16_t x, uint16_t y, const char *s,
                          uint16_t color, uint16_t bg, uint8_t scale)
{
    uint16_t cx = x;

    if (!tft_ready || s == RT_NULL) return;
    if (scale == 0) scale = 1;

    while (*s != '\0')
    {
        tft_draw_char(cx, y, *s++, color, bg, scale);
        cx += 6 * scale;
        if (cx >= TFT_GC9A01_WIDTH) break;
    }
}

static void tft_gc9a01_hw_reset(void)
{
    tft_gpio_write(TFT_GC9A01_PIN_RST, PIN_HIGH);
    tft_delay_ms(20);
    tft_gpio_write(TFT_GC9A01_PIN_RST, PIN_LOW);
    tft_delay_ms(20);
    tft_gpio_write(TFT_GC9A01_PIN_RST, PIN_HIGH);
    tft_delay_ms(120);
}

rt_err_t tft_gc9a01_init(void)
{
    uint8_t d[16];

    rt_pin_mode(TFT_GC9A01_PIN_SCL, PIN_MODE_OUTPUT);
    rt_pin_mode(TFT_GC9A01_PIN_SDA, PIN_MODE_OUTPUT);
    rt_pin_mode(TFT_GC9A01_PIN_CS,  PIN_MODE_OUTPUT);
    rt_pin_mode(TFT_GC9A01_PIN_DC,  PIN_MODE_OUTPUT);
    rt_pin_mode(TFT_GC9A01_PIN_RST, PIN_MODE_OUTPUT);

    tft_gpio_write(TFT_GC9A01_PIN_CS, PIN_HIGH);
    tft_gpio_write(TFT_GC9A01_PIN_SCL, PIN_HIGH);
    tft_gpio_write(TFT_GC9A01_PIN_SDA, PIN_HIGH);
    tft_gc9a01_hw_reset();

    tft_write_cmd(0xEF);
    d[0] = 0x14; tft_cmd_data(0xEB, d, 1);
    tft_write_cmd(0xFE);
    tft_write_cmd(0xEF);
    d[0] = 0x40; tft_cmd_data(0x84, d, 1);
    d[0] = 0xFF; tft_cmd_data(0x85, d, 1);
    d[0] = 0xFF; tft_cmd_data(0x86, d, 1);
    d[0] = 0x0A; tft_cmd_data(0x88, d, 1);
    d[0] = 0x21; tft_cmd_data(0x89, d, 1);
    d[0] = 0x00; tft_cmd_data(0x8A, d, 1);
    d[0] = 0x80; tft_cmd_data(0x8B, d, 1);
    d[0] = 0x01; tft_cmd_data(0x8C, d, 1);
    d[0] = 0x01; tft_cmd_data(0x8D, d, 1);
    d[0] = 0xFF; tft_cmd_data(0x8E, d, 1);
    d[0] = 0xFF; tft_cmd_data(0x8F, d, 1);
    d[0] = 0x00; d[1] = 0x20; tft_cmd_data(0xB6, d, 2);
    d[0] = 0x08; tft_cmd_data(0x36, d, 1);
    d[0] = 0x05; tft_cmd_data(0x3A, d, 1);
    d[0] = 0x08; d[1] = 0x08; d[2] = 0x08; d[3] = 0x08; tft_cmd_data(0x90, d, 4);
    d[0] = 0x06; tft_cmd_data(0xBD, d, 1);
    d[0] = 0x00; tft_cmd_data(0xBC, d, 1);
    d[0] = 0x13; tft_cmd_data(0xC3, d, 1);
    d[0] = 0x13; tft_cmd_data(0xC4, d, 1);
    d[0] = 0x22; tft_cmd_data(0xC9, d, 1);
    d[0] = 0x11; tft_cmd_data(0xBE, d, 1);
    d[0] = 0x10; d[1] = 0x0E; tft_cmd_data(0xE1, d, 2);
    d[0] = 0x21; d[1] = 0x0C; d[2] = 0x02; tft_cmd_data(0xDF, d, 3);
    d[0] = 0x45; d[1] = 0x09; d[2] = 0x08; d[3] = 0x08; d[4] = 0x26; d[5] = 0x2A; tft_cmd_data(0xF0, d, 6);
    d[0] = 0x43; d[1] = 0x70; d[2] = 0x72; d[3] = 0x36; d[4] = 0x37; d[5] = 0x6F; tft_cmd_data(0xF1, d, 6);
    d[0] = 0x45; d[1] = 0x09; d[2] = 0x08; d[3] = 0x08; d[4] = 0x26; d[5] = 0x2A; tft_cmd_data(0xF2, d, 6);
    d[0] = 0x43; d[1] = 0x70; d[2] = 0x72; d[3] = 0x36; d[4] = 0x37; d[5] = 0x6F; tft_cmd_data(0xF3, d, 6);
    d[0] = 0x1B; d[1] = 0x0B; tft_cmd_data(0xED, d, 2);
    d[0] = 0x77; tft_cmd_data(0xAE, d, 1);
    d[0] = 0x63; tft_cmd_data(0xCD, d, 1);

    tft_write_cmd(0x35);
    tft_write_cmd(0x21);
    tft_write_cmd(0x11);
    tft_delay_ms(120);
    tft_write_cmd(0x29);
    tft_delay_ms(20);

    tft_ready = RT_TRUE;
    tft_gc9a01_fill(TFT_COLOR_BLACK);
    return RT_EOK;
}

void tft_gc9a01_show_status(const char *fatigue_text,
                            const char *action_text,
                            uint32_t rep_count,
                            uint16_t fatigue_score)
{
    char buf[20];
    uint16_t fatigue_color;

    if (!tft_ready) return;

    fatigue_color = (fatigue_text != RT_NULL && fatigue_text[0] == 'F' &&
                     fatigue_text[1] == 'A') ? TFT_COLOR_RED : TFT_COLOR_GREEN;

    tft_gc9a01_fill_rect(58, 20, 124, 20, TFT_COLOR_BLACK);
    tft_gc9a01_draw_text(66, 24, "KNEE FIT", TFT_COLOR_CYAN, TFT_COLOR_BLACK, 2);

    tft_gc9a01_fill_rect(32, 58, 176, 30, TFT_COLOR_BLACK);
    tft_gc9a01_draw_text(48, 64, fatigue_text, fatigue_color, TFT_COLOR_BLACK, 3);

    tft_gc9a01_fill_rect(28, 112, 184, 24, TFT_COLOR_BLACK);
    rt_snprintf(buf, sizeof(buf), "ACT:%s", action_text ? action_text : "UNKNOWN");
    tft_gc9a01_draw_text(34, 118, buf, TFT_COLOR_WHITE, TFT_COLOR_BLACK, 2);

    tft_gc9a01_fill_rect(40, 148, 168, 34, TFT_COLOR_BLACK);
    rt_snprintf(buf, sizeof(buf), "CNT:%lu", (unsigned long)rep_count);
    tft_gc9a01_draw_text(46, 154, buf, TFT_COLOR_YELLOW, TFT_COLOR_BLACK, 3);

    tft_gc9a01_fill_rect(76, 198, 98, 24, TFT_COLOR_BLACK);
    rt_snprintf(buf, sizeof(buf), "F:%u%%", (unsigned)fatigue_score);
    tft_gc9a01_draw_text(82, 204, buf, TFT_COLOR_GRAY, TFT_COLOR_BLACK, 2);
}

#ifdef FINSH_USING_MSH
static void tft_test(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    rt_kprintf("Legacy software TFT driver is disabled. Use lcd_test.\n");
}
MSH_CMD_EXPORT(tft_test, redirect to the hardware SPI LCD test);
#endif
