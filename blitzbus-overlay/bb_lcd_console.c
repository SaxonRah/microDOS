/* blitzBUS v0.4: 80x25 DOS COM mirror + live diagnostic HUD.
 * Panel: 480x320 ST7796S. MADCTL=0xE8 supplied by CMake (MicroConsole default).
 * COM remains the input and debug transport; framebuffer is only a mirror.
 */
#include "bb_lcd_console.h"
#include "mr_pico_ili9341.h"
#include "gfx_font5x7.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#define COLS 80u
#define ROWS 25u
#define CW 6u
#define CH 8u
#define TEXT_H (ROWS*CH)
#define HUD_Y TEXT_H
#define HUD_H (320u-HUD_Y)
static mr_pico_ili9341_t lcd = {
    .spi=MR_LCD_SPI, .pin_miso=MR_LCD_PIN_MISO, .pin_cs=MR_LCD_PIN_CS,
    .pin_sck=MR_LCD_PIN_SCK, .pin_mosi=MR_LCD_PIN_MOSI,
    .pin_rst=MR_LCD_PIN_RST, .pin_dc=MR_LCD_PIN_DC,
    .spi_baud_hz=MR_LCD_SPI_BAUD
};
static uint8_t cells[ROWS][COLS];
static uint8_t dirty[ROWS];
/* One 480x8 RGB565 strip, static rather than stack. */
static uint16_t pixels[COLS*CW*CH];
static unsigned row, col;
static unsigned long characters, paint_rows, batches;
static uint32_t next_status_ms;
static int active;
static void draw_glyph(uint16_t *dst,unsigned pitch,unsigned xx,unsigned yy,unsigned char c,uint16_t fg,uint16_t bg){
    const uint8_t *g=gfx_font5x7[(c>=32 && c<=127 ? c: '?')-32];
    for(unsigned gy=0;gy<8;gy++) for(unsigned gx=0;gx<6;gx++)
        dst[(yy+gy)*pitch+xx+gx]=(gy<7 && gx<5 && (g[gx]&(1u<<gy)))?fg:bg;
}
static void draw_row(unsigned r){
    for(unsigned x=0;x<COLS;x++) draw_glyph(pixels,COLS*CW,x*CW,0,cells[r][x],0xFFFFu,0x0000u);
    mr_pico_ili9341_flush(NULL,0,(int)(r*CH),480,CH,pixels,&lcd);
    paint_rows++;
}
static void status_row(unsigned index,const char *msg){
    memset(pixels,0,sizeof pixels);
    unsigned len=(unsigned)strlen(msg);
    if(len>COLS)len=COLS;
    for(unsigned x=0;x<len;x++)draw_glyph(pixels,COLS*CW,x*CW,0,(unsigned char)msg[x],0x07FFu,0x0000u);
    mr_pico_ili9341_flush(NULL,0,(int)(HUD_Y+index*CH),480,CH,pixels,&lcd);
}
static void status_update(void){
    char msg[81];
    uint32_t ms=to_ms_since_boot(get_absolute_time());
    status_row(0,"blitzBUS  |  RP2350 / ST7796S  |  SERIAL COM CONTROL");
    status_row(1,"CPU: microDOS AOT + interpreter  |  blitz86 DOS backend: NOT ACTIVE");
    snprintf(msg,sizeof msg,"Uptime %lus  |  DOS text 80x25  |  LCD 480x320 RGB565",(unsigned long)(ms/1000u));status_row(2,msg);
    snprintf(msg,sizeof msg,"Console chars %lu  |  rendered text rows %lu  |  flushes %lu",characters,paint_rows,batches);status_row(3,msg);
    status_row(4,"Memory: guest 1 MiB PSRAM / conventional 640 KiB (configured)");
    status_row(5,"USB: serial input enabled  |  audio: not yet integrated");
    status_row(6,"DOS/BIOS: microDOS  |  real blitz86 integration pending");
    status_row(7,"Controls: COM keyboard  |  Ctrl+] performance log (serial)");
    for(unsigned y=8;y<HUD_H/CH;y++){memset(pixels,0,sizeof pixels);mr_pico_ili9341_flush(NULL,0,(int)(HUD_Y+y*CH),480,CH,pixels,&lcd);}
}
void bb_lcd_console_init(void){
    memset(cells,' ',sizeof cells);memset(dirty,1,sizeof dirty);
    puts("[blitzBUS] LCD SPI init begin");fflush(stdout);
    mr_pico_ili9341_init(&lcd);puts("[blitzBUS] LCD SPI init PASS");fflush(stdout);
    puts("[blitzBUS] LCD panel init begin (MADCTL=0xE8)");fflush(stdout);
    mr_pico_ili9341_panel_init(&lcd);puts("[blitzBUS] LCD panel init PASS");fflush(stdout);
    puts("[blitzBUS] LCD clear begin");fflush(stdout);
    mr_pico_ili9341_fill_screen(&lcd,0x0000u,480,320);puts("[blitzBUS] LCD clear PASS");fflush(stdout);
    row=col=0;characters=paint_rows=batches=0;active=1;next_status_ms=0;
    bb_lcd_console_flush();
}
static void newline(void){
    col=0;if(row+1<ROWS){row++;return;}
    memmove(cells[0],cells[1],(ROWS-1u)*COLS);memset(cells[ROWS-1u],' ',COLS);
    memset(dirty,1,sizeof dirty);
}
void bb_lcd_console_write(const uint8_t *data,size_t count){
    if(!active)return;
    for(size_t i=0;i<count;i++){
        unsigned c=data[i];characters++;
        if(c=='\r'){col=0;continue;}
        if(c=='\n'){newline();continue;}
        if(c=='\b'){if(col)col--;cells[row][col]=' ';dirty[row]=1;continue;}
        if(c=='\t'){unsigned n=8u-(col&7u);while(n--){cells[row][col]=' ';dirty[row]=1;if(++col>=COLS)newline();}continue;}
        if(c<32)continue;
        if(col>=COLS)newline();
        cells[row][col]=(uint8_t)(c<=127?c:'?');dirty[row]=1;
        if(++col>=COLS)newline();
    }
}
void bb_lcd_console_flush(void){
    if(!active)return;
    batches++;
    for(unsigned r=0;r<ROWS;r++)if(dirty[r]){draw_row(r);dirty[r]=0;}
    uint32_t ms=to_ms_since_boot(get_absolute_time());
    if((int32_t)(ms-next_status_ms)>=0){status_update();next_status_ms=ms+1000u;}
}
