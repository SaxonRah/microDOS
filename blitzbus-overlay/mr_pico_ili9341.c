#include "mr_pico_ili9341.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/regs/spi.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"

#define ILI9341_NOP 0x00u
#define ILI9341_SWRESET 0x01u
#define ILI9341_SLPOUT 0x11u
#define ILI9341_DISPON 0x29u
#define ILI9341_CASET 0x2Au
#define ILI9341_RASET 0x2Bu
#define ILI9341_RAMWR 0x2Cu
#define ILI9341_MADCTL 0x36u
#define ILI9341_COLMOD 0x3Au
#define ILI9341_FRMCTR1 0xB1u

#define ILI9341_INIT_SPI_BAUD 8000000u

static inline void lcd_gpio_put_fast(unsigned int pin, unsigned int value) {
  if (pin < 32u) {
    if (value)
      sio_hw->gpio_set = 1u << pin;
    else
      sio_hw->gpio_clr = 1u << pin;
  } else {
    gpio_put(pin, value ? 1 : 0);
  }
}

static inline void lcd_select(mr_pico_ili9341_t *ctx) {
  lcd_gpio_put_fast(ctx->pin_cs, 0u);
}

static inline void lcd_deselect(mr_pico_ili9341_t *ctx) {
  lcd_gpio_put_fast(ctx->pin_cs, 1u);
}

static inline void lcd_dc_command(mr_pico_ili9341_t *ctx) {
  lcd_gpio_put_fast(ctx->pin_dc, 0u);
}

static inline void lcd_dc_data(mr_pico_ili9341_t *ctx) {
  lcd_gpio_put_fast(ctx->pin_dc, 1u);
}

static inline void lcd_set_spi_format(mr_pico_ili9341_t *ctx,
                                      unsigned int bits) {
  if (ctx->spi_format_bits != bits) {
    spi_set_format(ctx->spi, bits, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    ctx->spi_format_bits = bits;
  }
}

static void lcd_wait_spi_idle(mr_pico_ili9341_t *ctx) {
  while (spi_get_hw(ctx->spi)->sr & SPI_SSPSR_BSY_BITS) {
    tight_loop_contents();
  }
}

static void lcd_write_cmd8_selected(mr_pico_ili9341_t *ctx, uint8_t cmd) {
  lcd_dc_command(ctx);
  spi_write_blocking(ctx->spi, &cmd, 1);
}

static void lcd_write_data8_selected(mr_pico_ili9341_t *ctx,
                                     const uint8_t *data, int len) {
  lcd_dc_data(ctx);
  if (data && len > 0) {
    spi_write_blocking(ctx->spi, data, (size_t)len);
  }
}

static void lcd_write_cmd(mr_pico_ili9341_t *ctx, uint8_t cmd) {
  lcd_set_spi_format(ctx, 8u);
  lcd_select(ctx);
  lcd_write_cmd8_selected(ctx, cmd);
  lcd_wait_spi_idle(ctx);
  lcd_deselect(ctx);
}

static void lcd_write_cmd_data(mr_pico_ili9341_t *ctx, uint8_t cmd,
                               const uint8_t *data, int len) {
  lcd_set_spi_format(ctx, 8u);
  lcd_select(ctx);
  lcd_write_cmd8_selected(ctx, cmd);
  lcd_write_data8_selected(ctx, data, len);
  lcd_wait_spi_idle(ctx);
  lcd_deselect(ctx);
}

static void lcd_begin_window_write(mr_pico_ili9341_t *ctx, int x, int y, int w,
                                   int h) {
  uint16_t x0 = (uint16_t)(x + ctx->x_offset);
  uint16_t y0 = (uint16_t)(y + ctx->y_offset);
  uint16_t x1 = (uint16_t)(x + w - 1 + ctx->x_offset);
  uint16_t y1 = (uint16_t)(y + h - 1 + ctx->y_offset);
  uint8_t data[4];

  lcd_set_spi_format(ctx, 8u);
  lcd_select(ctx);

  lcd_write_cmd8_selected(ctx, ILI9341_CASET);
  data[0] = (uint8_t)(x0 >> 8);
  data[1] = (uint8_t)x0;
  data[2] = (uint8_t)(x1 >> 8);
  data[3] = (uint8_t)x1;
  lcd_write_data8_selected(ctx, data, 4);

  lcd_write_cmd8_selected(ctx, ILI9341_RASET);
  data[0] = (uint8_t)(y0 >> 8);
  data[1] = (uint8_t)y0;
  data[2] = (uint8_t)(y1 >> 8);
  data[3] = (uint8_t)y1;
  lcd_write_data8_selected(ctx, data, 4);

  lcd_write_cmd8_selected(ctx, ILI9341_RAMWR);
  lcd_dc_data(ctx);
  lcd_wait_spi_idle(ctx);
}

void mr_pico_ili9341_init(mr_pico_ili9341_t *ctx) {
  ctx->spi_format_bits = 0u;
  ctx->spi_baud_hz = spi_init(ctx->spi, ctx->spi_baud_hz);
  lcd_set_spi_format(ctx, 8u);

  gpio_set_function(ctx->pin_miso, GPIO_FUNC_SPI);
  gpio_set_function(ctx->pin_sck, GPIO_FUNC_SPI);
  gpio_set_function(ctx->pin_mosi, GPIO_FUNC_SPI);

  gpio_init(ctx->pin_dc);
  gpio_set_dir(ctx->pin_dc, GPIO_OUT);
  gpio_put(ctx->pin_dc, 1);

  gpio_init(ctx->pin_cs);
  gpio_set_dir(ctx->pin_cs, GPIO_OUT);
  gpio_put(ctx->pin_cs, 1);

  gpio_init(ctx->pin_rst);
  gpio_set_dir(ctx->pin_rst, GPIO_OUT);
  gpio_put(ctx->pin_rst, 1);

  ctx->dma_chan = (unsigned int)dma_claim_unused_channel(true);
  ctx->dma_active = 0u;
  ctx->spi_format_bits = 8u;

  ctx->dma_cfg16 = dma_channel_get_default_config(ctx->dma_chan);
  channel_config_set_transfer_data_size(&ctx->dma_cfg16, DMA_SIZE_16);
  channel_config_set_read_increment(&ctx->dma_cfg16, true);
  channel_config_set_write_increment(&ctx->dma_cfg16, false);
  channel_config_set_dreq(&ctx->dma_cfg16, spi_get_dreq(ctx->spi, true));
  channel_config_set_high_priority(&ctx->dma_cfg16, true);
}

void mr_pico_ili9341_panel_init(mr_pico_ili9341_t *ctx) {
  uint8_t data;
  uint32_t run_baud;

  /*
   * Keep the high SPI rate for framebuffer traffic, but initialize the panel
   * conservatively. A warm picotool reboot does not remove power from the
   * ILI9341, and this reset/sleep-out path is the only part that behaves
   * differently from a cold BOOTSEL flash.
   */
  run_baud = ctx->spi_baud_hz;
  (void)spi_set_baudrate(ctx->spi, ILI9341_INIT_SPI_BAUD);

  gpio_put(ctx->pin_rst, 0);
  sleep_ms(30);
  gpio_put(ctx->pin_rst, 1);
  sleep_ms(120);

  lcd_write_cmd(ctx, ILI9341_SWRESET);
  sleep_ms(150);
  lcd_write_cmd(ctx, ILI9341_SLPOUT);
  sleep_ms(150);

  data = 0x55u; /* RGB565 / 16bpp */
  lcd_write_cmd_data(ctx, ILI9341_COLMOD, &data, 1);
  sleep_ms(10);

  data = (uint8_t)MR_ILI9341_MADCTL;
  lcd_write_cmd_data(ctx, ILI9341_MADCTL, &data, 1);
  sleep_ms(10);

#if !MR_LCD_PANEL_ST7796S
#if (MR_ILI9341_FRMCTR1_DIVA != 0x00u) || (MR_ILI9341_FRMCTR1_RTNA != 0x1Bu)
  {
    uint8_t frmctr[2];
    frmctr[0] = (uint8_t)(MR_ILI9341_FRMCTR1_DIVA & 0x03u);
    frmctr[1] = (uint8_t)(MR_ILI9341_FRMCTR1_RTNA & 0x1Fu);
    lcd_write_cmd_data(ctx, ILI9341_FRMCTR1, frmctr, 2);
    sleep_ms(10);
  }
#endif
#endif

  lcd_write_cmd(ctx, ILI9341_DISPON);
  sleep_ms(120);

  /* Restore the established high-speed rate used by normal pixel transfers. */
  ctx->spi_baud_hz = spi_set_baudrate(ctx->spi, run_baud);
}

void mr_pico_ili9341_flush_begin(gfx_renderer_t *r, int x, int y, int w, int h,
                                 const gfx_color_t *pixels, void *user) {
  mr_pico_ili9341_t *ctx = (mr_pico_ili9341_t *)user;
  uint32_t count;

  (void)r;

  if (!ctx || !pixels || w <= 0 || h <= 0)
    return;

  if (ctx->dma_active) {
    mr_pico_ili9341_flush_wait(r, user);
  }

  lcd_begin_window_write(ctx, x, y, w, h);

  /* RGB565 is sent as one 16-bit SPI frame, MSB first. */
  lcd_set_spi_format(ctx, 16u);

  count = (uint32_t)((uint32_t)w * (uint32_t)h);
  dma_channel_configure(ctx->dma_chan, &ctx->dma_cfg16, &spi_get_hw(ctx->spi)->dr,
                        pixels, count, true);
  ctx->dma_active = 1u;
}

void mr_pico_ili9341_flush_wait(gfx_renderer_t *r, void *user) {
  mr_pico_ili9341_t *ctx = (mr_pico_ili9341_t *)user;

  (void)r;

  if (!ctx || !ctx->dma_active)
    return;

  dma_channel_wait_for_finish_blocking(ctx->dma_chan);
  lcd_wait_spi_idle(ctx);
  lcd_deselect(ctx);
  lcd_set_spi_format(ctx, 8u);
  ctx->dma_active = 0u;
}

void mr_pico_ili9341_flush(gfx_renderer_t *r, int x, int y, int w, int h,
                           const gfx_color_t *pixels, void *user) {
  mr_pico_ili9341_flush_begin(r, x, y, w, h, pixels, user);
  mr_pico_ili9341_flush_wait(r, user);
}

void mr_pico_ili9341_fill_screen(mr_pico_ili9341_t *ctx, gfx_color_t color,
                                 int w, int h) {
  static gfx_color_t line[MR_LCD_MAX_WIDTH];
  int x;
  int y;
  int line_w = w;

  if (line_w > MR_LCD_MAX_WIDTH)
    line_w = MR_LCD_MAX_WIDTH;

  for (x = 0; x < line_w; ++x)
    line[x] = color;

  for (y = 0; y < h; ++y) {
    mr_pico_ili9341_flush_begin(0, 0, y, line_w, 1, line, ctx);
    mr_pico_ili9341_flush_wait(0, ctx);
  }
}
