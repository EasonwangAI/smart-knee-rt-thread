#ifndef __TFT_GC9A01_H__
#define __TFT_GC9A01_H__

#include <rtthread.h>
#include <stdint.h>
#include <stm32f4xx.h>

#ifndef GET_PIN
#define __TFT_STM32_PORT(port)          GPIO##port##_BASE
#define GET_PIN(PORTx, PIN)             (rt_base_t)((16 * ((((rt_base_t)__TFT_STM32_PORT(PORTx)) - (rt_base_t)GPIOA_BASE) / 0x0400UL)) + (PIN))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Project wiring for the 1.28 inch GC9A01 round TFT.
 * SCL -> PA5, SDA -> PA7, CS -> PA4, DC -> PA3, RST -> PA2.
 */
#ifndef TFT_GC9A01_PIN_SCL
#define TFT_GC9A01_PIN_SCL              GET_PIN(A, 5)
#endif

#ifndef TFT_GC9A01_PIN_SDA
#define TFT_GC9A01_PIN_SDA              GET_PIN(A, 7)
#endif

#ifndef TFT_GC9A01_PIN_CS
#define TFT_GC9A01_PIN_CS               GET_PIN(A, 4)
#endif

#ifndef TFT_GC9A01_PIN_DC
#define TFT_GC9A01_PIN_DC               GET_PIN(A, 3)
#endif

#ifndef TFT_GC9A01_PIN_RST
#define TFT_GC9A01_PIN_RST              GET_PIN(A, 2)
#endif

#define TFT_GC9A01_WIDTH                240
#define TFT_GC9A01_HEIGHT               240

#define TFT_COLOR_BLACK                 0x0000
#define TFT_COLOR_WHITE                 0xFFFF
#define TFT_COLOR_RED                   0xF800
#define TFT_COLOR_GREEN                 0x07E0
#define TFT_COLOR_BLUE                  0x001F
#define TFT_COLOR_CYAN                  0x07FF
#define TFT_COLOR_YELLOW                0xFFE0
#define TFT_COLOR_GRAY                  0x8410

rt_err_t tft_gc9a01_init(void);
void tft_gc9a01_fill(uint16_t color);
void tft_gc9a01_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void tft_gc9a01_draw_text(uint16_t x, uint16_t y, const char *s,
                          uint16_t color, uint16_t bg, uint8_t scale);
void tft_gc9a01_show_status(const char *fatigue_text,
                            const char *action_text,
                            uint32_t rep_count,
                            uint16_t fatigue_score);

#ifdef __cplusplus
}
#endif

#endif /* __TFT_GC9A01_H__ */
