#ifndef MR_PICO_ILI9341_H
#define MR_PICO_ILI9341_H

#include "gfx.h"
#include "hardware/dma.h"
#include "hardware/spi.h"

#ifndef MR_LCD_SPI
#define MR_LCD_SPI spi0
#endif

#ifndef MR_LCD_PIN_MISO
#define MR_LCD_PIN_MISO 4
#endif
#ifndef MR_LCD_PIN_CS
#define MR_LCD_PIN_CS 5
#endif
#ifndef MR_LCD_PIN_SCK
#define MR_LCD_PIN_SCK 6
#endif
#ifndef MR_LCD_PIN_MOSI
#define MR_LCD_PIN_MOSI 7
#endif
#ifndef MR_LCD_PIN_RST
#define MR_LCD_PIN_RST 8
#endif
#ifndef MR_LCD_PIN_DC
#define MR_LCD_PIN_DC 9
#endif
#ifndef MR_LCD_SPI_BAUD
#define MR_LCD_SPI_BAUD 40000000u
#endif

#ifndef MR_ILI9341_MADCTL
#define MR_ILI9341_MADCTL 0x48u
#endif

/* Historical driver name retained for compatibility.  The transport and
 * common DCS commands are also used by the ST7796S panel path. */
#ifndef MR_LCD_PANEL_ST7796S
#define MR_LCD_PANEL_ST7796S 0
#endif

#ifndef MR_LCD_MAX_WIDTH
#define MR_LCD_MAX_WIDTH 480
#endif

/* Panel refresh rate (FRMCTR1, 0xB1).
 *
 * The panel scans its own GRAM independently of how fast we write to it. At
 * the reset default of about 70 Hz, presentations beyond ~70 FPS are
 * overwritten before they are ever scanned out, so the extra frames cost SPI
 * bandwidth and buy nothing visible.
 *
 * Frame rate is fosc / (clocks-per-line * (lines + VFP + VBP)). RTNA sets
 * clocks per line and DIVA divides fosc. Lower RTNA is faster:
 *
 *     RTNA  0x1B = 70 Hz (reset default)   0x18 = 79 Hz
 *           0x19 = 76 Hz                   0x13 = 100 Hz
 *           0x16 = 86 Hz                   0x10 = 119 Hz (fastest)
 *
 * Defaults reproduce the reset state exactly, so this changes nothing until
 * you ask it to. Raising the refresh rate shortens the blanking interval and
 * makes tearing more likely, not less -- it is only worth doing alongside a
 * presentation rate that actually exceeds 70 FPS.
 */
#ifndef MR_ILI9341_FRMCTR1_DIVA
#define MR_ILI9341_FRMCTR1_DIVA 0x00u
#endif
#ifndef MR_ILI9341_FRMCTR1_RTNA
#define MR_ILI9341_FRMCTR1_RTNA 0x1Bu
#endif

typedef struct mr_pico_ili9341 {
  spi_inst_t *spi;
  unsigned int dma_chan;
  unsigned int pin_miso;
  unsigned int pin_cs;
  unsigned int pin_sck;
  unsigned int pin_mosi;
  unsigned int pin_rst;
  unsigned int pin_dc;
  unsigned int spi_baud_hz;
  int x_offset;
  int y_offset;
  unsigned int dma_active;
  unsigned int spi_format_bits;
  dma_channel_config dma_cfg16;
} mr_pico_ili9341_t;

void mr_pico_ili9341_init(mr_pico_ili9341_t *ctx);
void mr_pico_ili9341_panel_init(mr_pico_ili9341_t *ctx);
void mr_pico_ili9341_fill_screen(mr_pico_ili9341_t *ctx, gfx_color_t color,
                                 int w, int h);

void mr_pico_ili9341_flush(gfx_renderer_t *r, int x, int y, int w, int h,
                           const gfx_color_t *pixels, void *user);

void mr_pico_ili9341_flush_begin(gfx_renderer_t *r, int x, int y, int w, int h,
                                 const gfx_color_t *pixels, void *user);

void mr_pico_ili9341_flush_wait(gfx_renderer_t *r, void *user);

#endif
