#ifndef BB_LCD_CONSOLE_H
#define BB_LCD_CONSOLE_H
#include <stddef.h>
#include <stdint.h>
void bb_lcd_console_init(void);
void bb_lcd_console_write(const uint8_t *data, size_t size);
void bb_lcd_console_flush(void);
#endif
