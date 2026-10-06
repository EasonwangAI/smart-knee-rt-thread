#ifndef __GC9A01_H__
#define __GC9A01_H__

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_common.h"
#include "drv_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GC9A01_WIDTH                    240
#define GC9A01_HEIGHT                   240

#ifndef GC9A01_SPI_BUS_NAME
#define GC9A01_SPI_BUS_NAME             "spi1"
#endif

#ifndef GC9A01_SPI_DEVICE_NAME
#define GC9A01_SPI_DEVICE_NAME          "gc9a01"
#endif

#ifndef GC9A01_SPI_MAX_HZ
#define GC9A01_SPI_MAX_HZ               (2 * 1000 * 1000)
#endif

/* Wiring used by this project:
 * SCL/SCK -> PA5, SDA/MOSI -> PA7, CS -> PA4, DC -> PA3, RST -> PA2.
 * The display is write-only, so PA6/MISO can be left unconnected.
 */
#ifndef GC9A01_PIN_RST
#define GC9A01_PIN_RST                  GET_PIN(A, 2)
#endif

#ifndef GC9A01_PIN_DC
#define GC9A01_PIN_DC                   GET_PIN(A, 3)
#endif

#ifndef GC9A01_PIN_CS
#define GC9A01_PIN_CS                   GET_PIN(A, 4)
#endif

#ifndef GC9A01_CS_GPIO_PORT
#define GC9A01_CS_GPIO_PORT             GPIOA
#endif

#ifndef GC9A01_CS_GPIO_PIN
#define GC9A01_CS_GPIO_PIN              GPIO_PIN_4
#endif

#define GC9A01_BLACK                    0x0000
#define GC9A01_WHITE                    0xFFFF
#define GC9A01_RED                      0xF800
#define GC9A01_GREEN                    0x07E0
#define GC9A01_BLUE                     0x001F
#define GC9A01_YELLOW                   0xFFE0
#define GC9A01_CYAN                     0x07FF
#define GC9A01_GRAY                     0x8410
#define GC9A01_ORANGE                   0xFD20

rt_err_t gc9a01_init(void);
rt_err_t gc9a01_recover(void);
rt_bool_t gc9a01_is_ready(void);
rt_bool_t gc9a01_needs_recovery(void);
void gc9a01_clear(rt_uint16_t color);
void gc9a01_fill_rect(rt_uint16_t x, rt_uint16_t y,
                      rt_uint16_t w, rt_uint16_t h,
                      rt_uint16_t color);
void gc9a01_draw_text(rt_uint16_t x, rt_uint16_t y, const char *text,
                      rt_uint16_t fg, rt_uint16_t bg, rt_uint8_t scale);
void gc9a01_show_fit(rt_uint32_t total_count, rt_uint32_t squat_count,
                     rt_uint32_t deadlift_count, rt_uint16_t fatigue_score,
                     rt_uint8_t fatigue_alert, const char *action,
                     const char *emg_status);

#ifdef __cplusplus
}
#endif

#endif /* __GC9A01_H__ */
