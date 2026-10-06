#include "gc9a01.h"

#include <stdlib.h>
#include <string.h>

#define DBG_TAG "gc9a01"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

#ifdef FINSH_USING_MSH
#include <finsh.h>
#endif

static struct rt_spi_device *lcd_spi = RT_NULL;
static rt_bool_t lcd_ready = RT_FALSE;
static rt_bool_t fit_layout_ready = RT_FALSE;
static volatile rt_bool_t lcd_spi_fault = RT_FALSE;
static volatile rt_uint32_t lcd_spi_error_count = 0;
static rt_uint8_t lcd_fill_line[GC9A01_WIDTH * 2];

static void lcd_delay_ms(rt_uint32_t ms)
{
    rt_thread_mdelay(ms);
}

static rt_err_t lcd_spi_attach(void)
{
    rt_err_t result;

    lcd_spi = (struct rt_spi_device *)rt_device_find(GC9A01_SPI_DEVICE_NAME);
    if (lcd_spi == RT_NULL)
    {
        result = rt_hw_spi_device_attach(GC9A01_SPI_BUS_NAME,
                                         GC9A01_SPI_DEVICE_NAME,
                                         GC9A01_CS_GPIO_PORT,
                                         GC9A01_CS_GPIO_PIN);
        if (result != RT_EOK)
        {
            LOG_E("attach %s to %s failed: %d",
                  GC9A01_SPI_DEVICE_NAME, GC9A01_SPI_BUS_NAME, result);
            return result;
        }

        lcd_spi = (struct rt_spi_device *)rt_device_find(GC9A01_SPI_DEVICE_NAME);
    }

    if (lcd_spi == RT_NULL)
    {
        LOG_E("can't find spi device %s", GC9A01_SPI_DEVICE_NAME);
        return -RT_ENOSYS;
    }

    struct rt_spi_configuration cfg;
    cfg.data_width = 8;
    cfg.mode = RT_SPI_MASTER | RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz = GC9A01_SPI_MAX_HZ;

    return rt_spi_configure(lcd_spi, &cfg);
}

static rt_bool_t lcd_send_checked(const rt_uint8_t *data, rt_size_t len)
{
    rt_size_t sent;

    if (lcd_spi == RT_NULL || data == RT_NULL || len == 0) return RT_FALSE;

    sent = rt_spi_send(lcd_spi, data, len);
    if (sent != len)
    {
        lcd_spi_error_count++;
        if (!lcd_spi_fault)
        {
            rt_kprintf("TFT,SPI_ERROR,count=%lu,sent=%lu,expected=%lu\n",
                       (unsigned long)lcd_spi_error_count,
                       (unsigned long)sent,
                       (unsigned long)len);
        }
        lcd_spi_fault = RT_TRUE;
        return RT_FALSE;
    }

    return RT_TRUE;
}

static rt_bool_t lcd_write_cmd(rt_uint8_t cmd)
{
    if (lcd_spi == RT_NULL) return RT_FALSE;

    rt_pin_write(GC9A01_PIN_DC, PIN_LOW);
    return lcd_send_checked(&cmd, 1);
}

static rt_bool_t lcd_write_data(const rt_uint8_t *data, rt_size_t len)
{
    if (lcd_spi == RT_NULL || data == RT_NULL || len == 0) return RT_FALSE;

    rt_pin_write(GC9A01_PIN_DC, PIN_HIGH);
    return lcd_send_checked(data, len);
}

static rt_bool_t lcd_write_u8(rt_uint8_t data)
{
    return lcd_write_data(&data, 1);
}

static rt_bool_t lcd_cmd_data(rt_uint8_t cmd, const rt_uint8_t *data, rt_size_t len)
{
    if (!lcd_write_cmd(cmd)) return RT_FALSE;
    return lcd_write_data(data, len);
}

static rt_bool_t lcd_set_window(rt_uint16_t x0, rt_uint16_t y0,
                                rt_uint16_t x1, rt_uint16_t y1)
{
    rt_uint8_t data[4];

    data[0] = (rt_uint8_t)(x0 >> 8);
    data[1] = (rt_uint8_t)(x0 & 0xff);
    data[2] = (rt_uint8_t)(x1 >> 8);
    data[3] = (rt_uint8_t)(x1 & 0xff);
    if (!lcd_cmd_data(0x2A, data, sizeof(data))) return RT_FALSE;

    data[0] = (rt_uint8_t)(y0 >> 8);
    data[1] = (rt_uint8_t)(y0 & 0xff);
    data[2] = (rt_uint8_t)(y1 >> 8);
    data[3] = (rt_uint8_t)(y1 & 0xff);
    if (!lcd_cmd_data(0x2B, data, sizeof(data))) return RT_FALSE;

    return lcd_write_cmd(0x2C);
}

static void lcd_reset(void)
{
    rt_pin_write(GC9A01_PIN_RST, PIN_HIGH);
    lcd_delay_ms(10);
    rt_pin_write(GC9A01_PIN_RST, PIN_LOW);
    lcd_delay_ms(20);
    rt_pin_write(GC9A01_PIN_RST, PIN_HIGH);
    lcd_delay_ms(120);
}

static void lcd_init_sequence(void)
{
    static const rt_uint8_t b6[] = {0x00, 0x00};
    static const rt_uint8_t ff[] = {0x60, 0x01, 0x04};
    static const rt_uint8_t e1[] = {0x10, 0x0E};
    static const rt_uint8_t df[] = {0x21, 0x0C, 0x02};
    static const rt_uint8_t f0[] = {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A};
    static const rt_uint8_t f1[] = {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F};
    static const rt_uint8_t ed[] = {0x1B, 0x0B};
    static const rt_uint8_t cd[] = {0x63};
    static const rt_uint8_t c70[] = {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03};
    static const rt_uint8_t c62[] = {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70};
    static const rt_uint8_t c63[] = {0x18, 0x11, 0x71, 0xF1, 0x70};
    static const rt_uint8_t c64[] = {0x18, 0x13, 0x71, 0xF3, 0x70};
    static const rt_uint8_t c66[] = {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00};
    static const rt_uint8_t c67[] = {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98};
    static const rt_uint8_t c74[] = {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00};
    static const rt_uint8_t c98[] = {0x3E, 0x07};

    lcd_write_cmd(0xEF);
    lcd_cmd_data(0xEB, (const rt_uint8_t *)"\x14", 1);
    lcd_write_cmd(0xFE);
    lcd_write_cmd(0xEF);
    lcd_cmd_data(0xEB, (const rt_uint8_t *)"\x14", 1);
    lcd_cmd_data(0x84, (const rt_uint8_t *)"\x40", 1);
    lcd_cmd_data(0x85, (const rt_uint8_t *)"\xFF", 1);
    lcd_cmd_data(0x86, (const rt_uint8_t *)"\xFF", 1);
    lcd_cmd_data(0x87, (const rt_uint8_t *)"\xFF", 1);
    lcd_cmd_data(0x88, (const rt_uint8_t *)"\x0A", 1);
    lcd_cmd_data(0x89, (const rt_uint8_t *)"\x21", 1);
    lcd_cmd_data(0x8A, (const rt_uint8_t *)"\x00", 1);
    lcd_cmd_data(0x8B, (const rt_uint8_t *)"\x80", 1);
    lcd_cmd_data(0x8C, (const rt_uint8_t *)"\x01", 1);
    lcd_cmd_data(0x8D, (const rt_uint8_t *)"\x01", 1);
    lcd_cmd_data(0x8E, (const rt_uint8_t *)"\xFF", 1);
    lcd_cmd_data(0x8F, (const rt_uint8_t *)"\xFF", 1);

    lcd_cmd_data(0xB6, b6, sizeof(b6));
    lcd_cmd_data(0x36, (const rt_uint8_t *)"\x88", 1);
    lcd_cmd_data(0x3A, (const rt_uint8_t *)"\x05", 1);
    lcd_cmd_data(0x90, (const rt_uint8_t *)"\x08\x08\x08\x08", 4);
    lcd_cmd_data(0xBD, (const rt_uint8_t *)"\x06", 1);
    lcd_cmd_data(0xBC, (const rt_uint8_t *)"\x00", 1);
    lcd_cmd_data(0xFF, ff, sizeof(ff));
    lcd_cmd_data(0xC3, (const rt_uint8_t *)"\x13", 1);
    lcd_cmd_data(0xC4, (const rt_uint8_t *)"\x13", 1);
    lcd_cmd_data(0xC9, (const rt_uint8_t *)"\x22", 1);
    lcd_cmd_data(0xBE, (const rt_uint8_t *)"\x11", 1);
    lcd_cmd_data(0xE1, e1, sizeof(e1));
    lcd_cmd_data(0xDF, df, sizeof(df));
    lcd_cmd_data(0xF0, f0, sizeof(f0));
    lcd_cmd_data(0xF1, f1, sizeof(f1));
    lcd_cmd_data(0xF2, f0, sizeof(f0));
    lcd_cmd_data(0xF3, f1, sizeof(f1));
    lcd_cmd_data(0xED, ed, sizeof(ed));
    lcd_cmd_data(0xAE, (const rt_uint8_t *)"\x77", 1);
    lcd_cmd_data(0xCD, cd, sizeof(cd));
    lcd_cmd_data(0x70, c70, sizeof(c70));
    lcd_cmd_data(0xE8, (const rt_uint8_t *)"\x34", 1);
    lcd_cmd_data(0x62, c62, sizeof(c62));
    lcd_cmd_data(0x63, c63, sizeof(c63));
    lcd_cmd_data(0x64, c64, sizeof(c64));
    lcd_cmd_data(0x66, c66, sizeof(c66));
    lcd_cmd_data(0x67, c67, sizeof(c67));
    lcd_cmd_data(0x74, c74, sizeof(c74));
    lcd_cmd_data(0x98, c98, sizeof(c98));

    lcd_write_cmd(0x35);
    lcd_write_u8(0x00);
    lcd_write_cmd(0x21);
    lcd_write_cmd(0x11);
    lcd_delay_ms(120);
    lcd_write_cmd(0x29);
    lcd_delay_ms(20);
}

rt_err_t gc9a01_init(void)
{
    rt_err_t result;

    if (lcd_ready && !lcd_spi_fault) return RT_EOK;

    lcd_ready = RT_FALSE;
    fit_layout_ready = RT_FALSE;
    lcd_spi_fault = RT_FALSE;

    rt_pin_mode(GC9A01_PIN_RST, PIN_MODE_OUTPUT);
    rt_pin_mode(GC9A01_PIN_DC, PIN_MODE_OUTPUT);
    rt_pin_write(GC9A01_PIN_DC, PIN_HIGH);

    result = lcd_spi_attach();
    if (result != RT_EOK)
    {
        return result;
    }

    lcd_reset();
    lcd_init_sequence();
    if (lcd_spi_fault)
    {
        return -RT_EIO;
    }
    lcd_ready = RT_TRUE;
    gc9a01_clear(GC9A01_BLACK);

    if (lcd_spi_fault)
    {
        lcd_ready = RT_FALSE;
        return -RT_EIO;
    }

    return RT_EOK;
}

rt_err_t gc9a01_recover(void)
{
    lcd_ready = RT_FALSE;
    fit_layout_ready = RT_FALSE;
    lcd_spi_fault = RT_FALSE;
    return gc9a01_init();
}

rt_bool_t gc9a01_is_ready(void)
{
    return lcd_ready;
}

rt_bool_t gc9a01_needs_recovery(void)
{
    return lcd_spi_fault;
}

void gc9a01_fill_rect(rt_uint16_t x, rt_uint16_t y,
                      rt_uint16_t w, rt_uint16_t h,
                      rt_uint16_t color)
{
    rt_size_t bytes;

    if (!lcd_ready || lcd_spi_fault || w == 0 || h == 0) return;
    if (x >= GC9A01_WIDTH || y >= GC9A01_HEIGHT) return;
    if (x + w > GC9A01_WIDTH) w = GC9A01_WIDTH - x;
    if (y + h > GC9A01_HEIGHT) h = GC9A01_HEIGHT - y;

    bytes = (rt_size_t)w * 2;
    for (rt_size_t i = 0; i < bytes; i += 2)
    {
        lcd_fill_line[i] = (rt_uint8_t)(color >> 8);
        lcd_fill_line[i + 1] = (rt_uint8_t)(color & 0xff);
    }

    for (rt_uint16_t row = 0; row < h; row++)
    {
        if (!lcd_set_window(x, y + row, x + w - 1, y + row)) break;
        rt_pin_write(GC9A01_PIN_DC, PIN_HIGH);
        if (!lcd_send_checked(lcd_fill_line, bytes)) break;
    }
}

void gc9a01_clear(rt_uint16_t color)
{
    gc9a01_fill_rect(0, 0, GC9A01_WIDTH, GC9A01_HEIGHT, color);
    fit_layout_ready = RT_FALSE;
}

static const rt_uint8_t *font5x7(char c)
{
    static const rt_uint8_t blank[5] = {0, 0, 0, 0, 0};
    static const rt_uint8_t qmark[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
    static const rt_uint8_t minus[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const rt_uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const rt_uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};

    static const rt_uint8_t digits[10][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E},
        {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4B, 0x31},
        {0x18, 0x14, 0x12, 0x7F, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3C, 0x4A, 0x49, 0x49, 0x30},
        {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1E},
    };
    static const rt_uint8_t letters[26][5] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E},
        {0x7F, 0x49, 0x49, 0x49, 0x36},
        {0x3E, 0x41, 0x41, 0x41, 0x22},
        {0x7F, 0x41, 0x41, 0x22, 0x1C},
        {0x7F, 0x49, 0x49, 0x49, 0x41},
        {0x7F, 0x09, 0x09, 0x09, 0x01},
        {0x3E, 0x41, 0x49, 0x49, 0x7A},
        {0x7F, 0x08, 0x08, 0x08, 0x7F},
        {0x00, 0x41, 0x7F, 0x41, 0x00},
        {0x20, 0x40, 0x41, 0x3F, 0x01},
        {0x7F, 0x08, 0x14, 0x22, 0x41},
        {0x7F, 0x40, 0x40, 0x40, 0x40},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F},
        {0x7F, 0x04, 0x08, 0x10, 0x7F},
        {0x3E, 0x41, 0x41, 0x41, 0x3E},
        {0x7F, 0x09, 0x09, 0x09, 0x06},
        {0x3E, 0x41, 0x51, 0x21, 0x5E},
        {0x7F, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31},
        {0x01, 0x01, 0x7F, 0x01, 0x01},
        {0x3F, 0x40, 0x40, 0x40, 0x3F},
        {0x1F, 0x20, 0x40, 0x20, 0x1F},
        {0x3F, 0x40, 0x38, 0x40, 0x3F},
        {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07},
        {0x61, 0x51, 0x49, 0x45, 0x43},
    };

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c == '-') return minus;
    if (c == ':') return colon;
    if (c == '/') return slash;
    if (c == ' ') return blank;
    return qmark;
}

static void draw_char(rt_uint16_t x, rt_uint16_t y, char c,
                      rt_uint16_t fg, rt_uint16_t bg, rt_uint8_t scale)
{
    const rt_uint8_t *bitmap = font5x7(c);
    rt_uint8_t line[6 * 4 * 2];
    rt_uint16_t width;
    rt_uint16_t height;

    if (scale == 0) scale = 1;
    if (scale > 4) scale = 4;

    width = 6 * scale;
    height = 8 * scale;
    if (x >= GC9A01_WIDTH || y >= GC9A01_HEIGHT) return;
    if (x + width > GC9A01_WIDTH || y + height > GC9A01_HEIGHT) return;

    if (lcd_spi_fault ||
        !lcd_set_window(x, y, x + width - 1, y + height - 1)) return;
    rt_pin_write(GC9A01_PIN_DC, PIN_HIGH);

    for (rt_uint8_t row = 0; row < 8; row++)
    {
        rt_uint16_t pos = 0;

        for (rt_uint8_t col = 0; col < 6; col++)
        {
            rt_uint8_t bits = col < 5 ? bitmap[col] : 0;
            rt_uint16_t color = (bits & (1 << row)) ? fg : bg;

            for (rt_uint8_t xs = 0; xs < scale; xs++)
            {
                line[pos++] = (rt_uint8_t)(color >> 8);
                line[pos++] = (rt_uint8_t)(color & 0xff);
            }
        }

        for (rt_uint8_t ys = 0; ys < scale; ys++)
        {
            if (!lcd_send_checked(line, pos)) return;
        }
    }
}

void gc9a01_draw_text(rt_uint16_t x, rt_uint16_t y, const char *text,
                      rt_uint16_t fg, rt_uint16_t bg, rt_uint8_t scale)
{
    if (!lcd_ready || lcd_spi_fault || text == RT_NULL) return;
    if (scale == 0) scale = 1;

    while (*text != '\0')
    {
        draw_char(x, y, *text, fg, bg, scale);
        x += 6 * scale;
        text++;
        if (x >= GC9A01_WIDTH - 6 * scale) break;
    }
}

static rt_uint16_t text_width_px(const char *text, rt_uint8_t scale)
{
    rt_uint16_t len = 0;

    if (text == RT_NULL) return 0;
    while (*text++ != '\0') len++;
    return (rt_uint16_t)(len * 6U * scale);
}

static void draw_text_center(rt_uint16_t y, const char *text,
                             rt_uint16_t fg, rt_uint16_t bg, rt_uint8_t scale)
{
    rt_uint16_t w = text_width_px(text, scale);
    rt_uint16_t x = 0;

    if (w < GC9A01_WIDTH)
    {
        x = (GC9A01_WIDTH - w) / 2;
    }
    gc9a01_draw_text(x, y, text, fg, bg, scale);
}

static void draw_row_center(rt_uint16_t y, rt_uint16_t h, const char *text,
                            rt_uint16_t fg, rt_uint8_t scale)
{
    gc9a01_fill_rect(8, y, 224, h, GC9A01_BLACK);
    draw_text_center(y, text, fg, GC9A01_BLACK, scale);
}

void gc9a01_show_fit(rt_uint32_t total_count, rt_uint32_t squat_count,
                     rt_uint32_t deadlift_count, rt_uint16_t fatigue_score,
                     rt_uint8_t fatigue_alert, const char *action,
                     const char *emg_status)
{
    char line[32];
    const char *action_text = action ? action : "UNK";
    const char *emg_text = emg_status ? emg_status : "NA";
    rt_uint16_t status_color = fatigue_alert ? GC9A01_RED : GC9A01_GREEN;
    rt_bool_t redraw_all;
    static rt_uint32_t prev_total = 0xffffffffU;
    static rt_uint32_t prev_squat = 0xffffffffU;
    static rt_uint32_t prev_deadlift = 0xffffffffU;
    static rt_uint16_t prev_fatigue = 0xffffU;
    static rt_uint8_t prev_alert = 0xffU;
    static char prev_action[12] = "";
    static char prev_emg[24] = "";

    if (!lcd_ready || lcd_spi_fault) return;

    redraw_all = !fit_layout_ready;
    if (!fit_layout_ready)
    {
        gc9a01_clear(GC9A01_BLACK);
        draw_text_center(14, "FIT MON", GC9A01_CYAN, GC9A01_BLACK, 2);
        fit_layout_ready = RT_TRUE;
    }

    if (redraw_all || total_count != prev_total)
    {
        rt_snprintf(line, sizeof(line), "TOTAL:%lu", (unsigned long)total_count);
        draw_row_center(44, 18, line, GC9A01_WHITE, 2);
        prev_total = total_count;
    }

    if (redraw_all || squat_count != prev_squat)
    {
        rt_snprintf(line, sizeof(line), "SQUAT:%lu", (unsigned long)squat_count);
        draw_row_center(72, 18, line, GC9A01_YELLOW, 2);
        prev_squat = squat_count;
    }

    if (redraw_all || deadlift_count != prev_deadlift)
    {
        rt_snprintf(line, sizeof(line), "DEAD:%lu", (unsigned long)deadlift_count);
        draw_row_center(100, 18, line, GC9A01_ORANGE, 2);
        prev_deadlift = deadlift_count;
    }

    if (redraw_all || strcmp(action_text, prev_action) != 0)
    {
        rt_snprintf(line, sizeof(line), "ACT:%s", action_text);
        draw_row_center(132, 18, line, GC9A01_CYAN, 2);
        strncpy(prev_action, action_text, sizeof(prev_action) - 1);
        prev_action[sizeof(prev_action) - 1] = '\0';
    }

    if (redraw_all || fatigue_score != prev_fatigue || fatigue_alert != prev_alert)
    {
        rt_snprintf(line, sizeof(line), "FAT:%s %u",
                    fatigue_alert ? "YES" : "NO",
                    (unsigned)fatigue_score);
        draw_row_center(164, 18, line, status_color, 2);
        prev_fatigue = fatigue_score;
        prev_alert = fatigue_alert;
    }

    if (redraw_all || strcmp(emg_text, prev_emg) != 0)
    {
        rt_snprintf(line, sizeof(line), "EMG:%s", emg_text);
        draw_row_center(196, 18, line, GC9A01_GRAY, 2);
        strncpy(prev_emg, emg_text, sizeof(prev_emg) - 1);
        prev_emg[sizeof(prev_emg) - 1] = '\0';
    }
}

#ifdef FINSH_USING_MSH
static void lcd_test(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    if (gc9a01_init() != RT_EOK)
    {
        rt_kprintf("lcd_test: GC9A01 init failed\n");
        return;
    }

    gc9a01_clear(GC9A01_RED);
    rt_thread_mdelay(300);
    gc9a01_clear(GC9A01_GREEN);
    rt_thread_mdelay(300);
    gc9a01_clear(GC9A01_BLUE);
    rt_thread_mdelay(300);
    gc9a01_clear(GC9A01_BLACK);
    gc9a01_draw_text(42, 80, "GC9A01", GC9A01_CYAN, GC9A01_BLACK, 3);
    gc9a01_draw_text(54, 126, "LCD OK", GC9A01_GREEN, GC9A01_BLACK, 3);
    rt_kprintf("lcd_test done\n");
}
MSH_CMD_EXPORT(lcd_test, test GC9A01 round TFT);
#endif
