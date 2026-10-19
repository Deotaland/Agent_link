/**
 * @file epd.c
 * @brief 墨水屏驱动：硬件 SPI2，引脚见 board_pins.h，屏幕型号见 epd.h
 */
#include <assert.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "board_pins.h"
#include "epd.h"

static const char *TAG = "epd";

#define EPD_SPI_HOST        SPI2_HOST
#define EPD_SPI_HZ          (4 * 1000 * 1000)
#if EPD_COLORS == 2
#define EPD_ROW_BYTES       (EPD_PANEL_W / 8)   /* 黑白：1 位一个像素 */
#else
#define EPD_ROW_BYTES       (EPD_PANEL_W / 2)   /* 六色：4 位一个像素 */
#endif
#define EPD_BUSY_TIMEOUT_MS 60000

/* 命令 */
#define PSR     0x00
#define PWRR    0x01
#define POF     0x02
#define POFS    0x03
#define PON     0x04
#define BTST1   0x05
#define BTST2   0x06
#define DSLP    0x07
#define BTST3   0x08
#define DTM     0x10    /* 六色屏的像素数据；黑白屏的"旧"数据 */
#define DRF     0x12
#define DTM2    0x13    /* 黑白屏的"新"数据 */
#define DUSPI   0x15
#define PTL     0x90    /* 局刷窗口 */
#define PTIN    0x91    /* 进入局刷模式 */
#define PTOUT   0x92    /* 退出局刷模式 */
#define CCSET   0xE0    /* bit1 TSFIX：用 TSSET 强制温度 */
#define TSSET   0xE5
#define PLL     0x30
#define CDI     0x50
#define TCON    0x60
#define TRES    0x61
#define VDCS    0x82
#define T_VDCS  0x84
#define PWS     0xE3

static spi_device_handle_t s_spi;
static uint8_t *s_row;      /* 一行像素（EPD_ROW_BYTES 字节），DMA 缓冲 */
static int s_busy_timeouts;
static bool s_dead;         /* 超时过一次就不再干等，直到 epd_probe() 确认屏又能响应 */

/* ================================================================
 *  SPI 底层
 * ================================================================ */
static void spi_send(int dc, const uint8_t *buf, size_t len)
{
    gpio_set_level(EPD_DC_GPIO, dc);     /* D/C#  0:command  1:data */
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void spi_send_byte(int dc, uint8_t value)
{
    gpio_set_level(EPD_DC_GPIO, dc);
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .tx_data = { value },
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void EPD_W21_WriteCMD(uint8_t command)
{
    spi_send_byte(0, command);
}

static void EPD_W21_WriteDATA(uint8_t data)
{
    spi_send_byte(1, data);
}

static void write_cmd_data(uint8_t command, const uint8_t *data, size_t len)
{
    EPD_W21_WriteCMD(command);
    for (size_t i = 0; i < len; i++) {
        EPD_W21_WriteDATA(data[i]);
    }
}

#define CMD(c, ...) do { \
        static const uint8_t _d[] = { __VA_ARGS__ }; \
        write_cmd_data((c), _d, sizeof(_d)); \
    } while (0)

/* 等 BUSY_N 变高（低 = 忙）。原例程是死等，这里加超时并让出 CPU，避免看门狗复位 */
static void lcd_chkstatus(void)
{
    if (s_dead) {
        return;
    }
    TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(EPD_BUSY_GPIO) == 0) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(EPD_BUSY_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "BUSY 超时 %d ms：检查屏幕排线、EPD_3V3，后面的等待全部跳过",
                     EPD_BUSY_TIMEOUT_MS);
            s_busy_timeouts++;
            s_dead = true;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGD(TAG, "busy %lu ms", (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - start));
#if EPD_PANEL == EPD_PANEL_3IN6
    vTaskDelay(pdMS_TO_TICKS(100));     /* 微雪驱动在 BUSY 释放后多等 100ms */
#endif
}

#if EPD_COLORS == 2
/* 黑白屏：往 cmd（DTM 旧 / DTM2 新）发整屏 1 位数据，1 白 0 黑；picData 为空时每字节填 fill */
static void send_frame_bw(uint8_t cmd, const unsigned char *picData, uint8_t fill);
/* 只发一个窗口：源极 h0~h1（8 对齐）、栅极 g0~g1 */
static void send_window_bw(uint8_t cmd, const unsigned char *picData, int h0, int h1, int g0, int g1);
#else
/* 六色屏：发送整屏 DTM 数据，picData 为空时整屏填 color */
static void send_frame(const unsigned char *picData, uint8_t color);
#endif

/* ================================================================
 *  7.3 寸 GDEP073E01（800x480）—— Good Display 原厂时序
 * ================================================================ */
#if EPD_PANEL == EPD_PANEL_7IN3

static void EPD_W21_Init(void)
{
    gpio_set_level(EPD_RST_GPIO, 0);     /* Module reset，至少 10ms */
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* slow */
void EPD_init(void)
{
    EPD_W21_Init();

    CMD(0xAA, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18);   /* CMDH */
    CMD(PWRR, 0x3F);
    CMD(PSR, 0x5F, 0x69);
    CMD(POFS, 0x00, 0x54, 0x00, 0x44);
    CMD(BTST1, 0x40, 0x1F, 0x1F, 0x2C);
    CMD(BTST2, 0x6F, 0x1F, 0x17, 0x49);
    CMD(BTST3, 0x6F, 0x1F, 0x1F, 0x22);
    CMD(PLL, 0x08);
    CMD(CDI, 0x3F);
    CMD(TCON, 0x02, 0x00);
    CMD(TRES, 0x03, 0x20, 0x01, 0xE0);               /* 800 x 480 */
    CMD(T_VDCS, 0x01);
    CMD(PWS, 0x2F);

    EPD_W21_WriteCMD(PON);
    lcd_chkstatus();
}

/* fast */
void EPD_init_fast(void)
{
    EPD_W21_Init();

    CMD(0xAA, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18);   /* CMDH */
    CMD(PWRR, 0x3F, 0x00, 0x32, 0x2A, 0x0E, 0x2A);
    CMD(PSR, 0x5F, 0x69);
    CMD(POFS, 0x00, 0x54, 0x00, 0x44);
    CMD(BTST1, 0x40, 0x1F, 0x1F, 0x2C);
    CMD(BTST2, 0x6F, 0x1F, 0x16, 0x25);
    CMD(BTST3, 0x6F, 0x1F, 0x1F, 0x22);
    CMD(0x13, 0x00, 0x04);                           /* IPC */
    CMD(PLL, 0x02);
    CMD(0x41, 0x00);                                 /* TSE */
    CMD(CDI, 0x3F);
    CMD(TCON, 0x02, 0x00);
    CMD(TRES, 0x03, 0x20, 0x01, 0xE0);               /* 800 x 480 */
    CMD(VDCS, 0x1E);
    CMD(T_VDCS, 0x01);
    CMD(0x86, 0x00);                                 /* AGID */
    CMD(PWS, 0x2F);
    CMD(0xE0, 0x00);                                 /* CCSET */
    CMD(0xE6, 0x00);                                 /* TSSET */

    EPD_W21_WriteCMD(PON);
    lcd_chkstatus();
}

void EPD_sleep(void)
{
    CMD(POF, 0x00);                                  /* power off */
    lcd_chkstatus();
    /* 原例程注释掉了 deep sleep：CMD(DSLP, 0xA5); */
}

/* EPD_init 里已经 PON，这里只刷新；下电由 EPD_sleep 完成 */
void PIC_display(const unsigned char *picData)
{
    send_frame(picData, 0);
    CMD(DRF, 0x00);
    esp_rom_delay_us(1000);                          /* !!!必须延时，至少 200us!!! */
    lcd_chkstatus();
}

void PIC_display_Clear(void)
{
    send_frame(NULL, EPD_WHITE);
    CMD(DRF, 0x00);
    esp_rom_delay_us(1000);
    lcd_chkstatus();
}

void EPD_Display_Color(uint8_t color)
{
    send_frame(NULL, color);

    EPD_W21_WriteCMD(PON);
    lcd_chkstatus();

    CMD(BTST2, 0x6F, 0x1F, 0x17, 0x49);              /* 20211212 Second setting */

    CMD(DRF, 0x00);
    lcd_chkstatus();

    CMD(POF, 0x00);
    lcd_chkstatus();
}

/* ================================================================
 *  3.6 寸 Waveshare (E)（400x600）—— 微雪官方时序
 * ================================================================ */
#elif EPD_PANEL == EPD_PANEL_3IN6

static void EPD_W21_Init(void)
{
    gpio_set_level(EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* 上电 -> 刷新 -> 下电 */
static void display_refresh(void)
{
    EPD_W21_WriteCMD(PON);
    lcd_chkstatus();
    vTaskDelay(pdMS_TO_TICKS(200));

    CMD(BTST2, 0x6F, 0x1F, 0x16, 0x29);              /* Second setting */
    vTaskDelay(pdMS_TO_TICKS(200));

    CMD(DRF, 0x00);
    lcd_chkstatus();

    CMD(POF, 0x00);
    lcd_chkstatus();
}

void EPD_init(void)
{
    EPD_W21_Init();
    lcd_chkstatus();
    vTaskDelay(pdMS_TO_TICKS(30));

    CMD(0xAA, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18);   /* CMDH */
    CMD(PWRR, 0x3F);
    CMD(PSR, 0x5F, 0x69);
    CMD(BTST1, 0x40, 0x1F, 0x1F, 0x2C);
    CMD(BTST3, 0x6F, 0x1F, 0x1F, 0x22);
    CMD(BTST2, 0x6F, 0x1F, 0x17, 0x17);              /* First setting */
    CMD(POFS, 0x00, 0x54, 0x00, 0x44);
    CMD(TCON, 0x02, 0x00);
    CMD(PLL, 0x08);
    CMD(CDI, 0x3F);
    CMD(TRES, 0x01, 0x90, 0x02, 0x58);               /* 400 x 600 */
    CMD(PWS, 0x2F);
    CMD(T_VDCS, 0x01);
    lcd_chkstatus();
}

void EPD_init_fast(void)
{
    EPD_init();
}

void EPD_sleep(void)
{
    CMD(DSLP, 0xA5);    /* deep sleep，唤醒需要重新 EPD_init() */
}

void PIC_display(const unsigned char *picData)
{
    send_frame(picData, 0);
    display_refresh();
}

void EPD_Display_Color(uint8_t color)
{
    send_frame(NULL, color);
    display_refresh();
}

void PIC_display_Clear(void)
{
    EPD_Display_Color(EPD_WHITE);
}

/* ================================================================
 *  4.0 寸 秋田微 E0400A01-AF-S（黑白 528x792）—— UC8179 命令集
 *
 *  规格书 11.2 的参考程序只给了"口令 → 上电 → 写数据 → 刷新 → 下电 → 休眠"，
 *  而且是别的屏的模板（没设分辨率，循环 5624 字节）。这里保留口令和流程，
 *  面板模式 / 分辨率 / 电压按 UC8179 黑白屏常用值补全：
 *    PWR  VGH/VGL ±20V、VSH/VSL ±15V（与规格书光学测试条件一致）
 *    PSR  0x1F：黑白模式，波形用 OTP（芯片默认 0x0F 是黑白红模式），镜像时改 epd.h 的 EPD_4IN0_PSR
 *    TRES 792 x 600：芯片横着扫（长边是源极），短边 600 根栅极全刷（屏上用了 528 根，
 *         偶数 G0~G598 在左 300 列、奇数 G1~G455 在右 228 列），竖放靠 EPD_ROTATION 90 转回 528x792
 *    CDI  0x29 0x07：新数据 1=白 0=黑，刷新后新数据自动拷到旧数据
 * ================================================================ */
#elif EPD_PANEL == EPD_PANEL_4IN0

static void EPD_W21_Init(void)
{
    gpio_set_level(EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));                   /* RST 释放后芯片内部初始化至少 1ms */
}

/* 复位 + 写寄存器，不上电（局刷要在上电前强制温度） */
static void init_regs(void)
{
    EPD_W21_Init();
    lcd_chkstatus();

    CMD(0x4D, 0x55);                                 /* 厂家口令（规格书 11.2） */
    CMD(0xF3, 0x0A);
    CMD(PWRR, 0x07, 0x07, 0x3F, 0x3F);
    CMD(PSR, EPD_4IN0_PSR);
    CMD(TRES, EPD_PANEL_W >> 8, EPD_PANEL_W & 0xFF, EPD_4IN0_GATES >> 8, EPD_4IN0_GATES & 0xFF);   /* 792 x 600 */
    CMD(DUSPI, 0x00);
    CMD(CDI, 0x29, 0x07);
    CMD(TCON, 0x22);
}

static void power_on(void)
{
    EPD_W21_WriteCMD(PON);
    lcd_chkstatus();
}

void EPD_init(void)
{
    init_regs();
    power_on();
}

void EPD_init_fast(void)
{
    EPD_init();         /* 规格书只有 OTP 全刷波形 */
}

void EPD_sleep(void)
{
    EPD_W21_WriteCMD(POF);
    lcd_chkstatus();
    vTaskDelay(pdMS_TO_TICKS(20));
    lcd_chkstatus();
    CMD(DSLP, 0xA5);    /* deep sleep，唤醒需要重新 EPD_init() */
}

static void bw_refresh(void)
{
    EPD_W21_WriteCMD(DRF);
    esp_rom_delay_us(1000);
    lcd_chkstatus();
}

/* 屏上当前画面（应用层格式，EPD_WIDTH x EPD_HEIGHT 字节），局刷时当"旧数据"、并补齐窗口里没变的部分 */
static uint8_t *s_shadow;

static uint8_t *shadow(void)
{
    if (!s_shadow) {
        s_shadow = heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_shadow) {
            memset(s_shadow, 0xFF, EPD_WIDTH * EPD_HEIGHT);
        }
    }
    return s_shadow;
}

static bool s_shadow_valid;     /* 全刷过一次后才知道屏上是什么 */

void PIC_display(const unsigned char *picData)
{
    send_frame_bw(DTM, NULL, 0xFF);                  /* 旧数据当作全白 */
    send_frame_bw(DTM2, picData, 0);
    bw_refresh();
    if (shadow()) {
        memcpy(s_shadow, picData, EPD_WIDTH * EPD_HEIGHT);
        s_shadow_valid = true;
    }
}

void EPD_Display_Color(uint8_t color)
{
    send_frame_bw(DTM, NULL, 0xFF);
    send_frame_bw(DTM2, NULL, color == EPD_WHITE ? 0xFF : 0x00);
    bw_refresh();
    if (shadow()) {
        memset(s_shadow, color == EPD_WHITE ? 0xFF : 0x00, EPD_WIDTH * EPD_HEIGHT);
        s_shadow_valid = true;
    }
}

void PIC_display_Clear(void)
{
    EPD_Display_Color(EPD_WHITE);
}

/* 屏上第 x 列（从左往右）由哪根栅极驱动：偶数 G0~G598 在左 300 列，奇数接着排右边 */
static int gate_of_col(int x)
{
    return x < EPD_4IN0_LEFT_COLS ? 2 * x : 2 * (x - EPD_4IN0_LEFT_COLS) + 1;
}

/* 栅极 g 在屏上第几列，不在屏上返回 -1 */
static int col_of_gate(int g)
{
    int x = (g % 2 == 0) ? g / 2 : EPD_4IN0_LEFT_COLS + g / 2;
    return x < EPD_WIDTH ? x : -1;
}

/* 局部刷新。picData 是整屏新画面，(x,y,w,h) 是变了的区域（应用层坐标）。
 * 芯片窗口：屏上的行 = 源极（按 8 对齐），屏上的列换算成栅极取最小~最大。
 * 因为栅极奇偶分在左右两边，一个窗口在屏上是左右两条，两条都按新画面发。 */
void epd_partial(const unsigned char *picData, int x, int y, int w, int h)
{
#if EPD_ROTATION != 90
    ESP_LOGW(TAG, "局刷只按竖放（EPD_ROTATION 90）算了窗口，改用全刷");
    EPD_init();
    PIC_display(picData);
    EPD_sleep();
    return;
#endif
    if (!shadow() || !s_shadow_valid) {
        ESP_LOGI(TAG, "还没全刷过，先全刷一次");
        EPD_init();
        PIC_display(picData);
        EPD_sleep();
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > EPD_WIDTH) { w = EPD_WIDTH - x; }
    if (y + h > EPD_HEIGHT) { h = EPD_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return;
    }

    int h0 = y & ~7, h1 = (y + h - 1) | 7;           /* 源极按 8 对齐 */
    int g0 = EPD_4IN0_GATES, g1 = -1;
    for (int c = x; c < x + w; c++) {
        int g = gate_of_col(c);
        g0 = g < g0 ? g : g0;
        g1 = g > g1 ? g : g1;
    }
    ESP_LOGD(TAG, "局刷 (%d,%d %dx%d) -> 源极 %d~%d 栅极 %d~%d", x, y, w, h, h0, h1, g0, g1);

    init_regs();
#if EPD_4IN0_PARTIAL_TEMP
    CMD(CCSET, 0x02);                                /* TSFIX：用强制温度选 OTP 里的另一套波形 */
    CMD(TSSET, EPD_4IN0_PARTIAL_TEMP);
#endif
    power_on();
    EPD_W21_WriteCMD(PTIN);
    const uint8_t win[] = {
        h0 >> 8, h0 & 0xF8, h1 >> 8, (h1 & 0xF8) | 0x07,
        g0 >> 8, g0 & 0xFF, g1 >> 8, g1 & 0xFF,
        0x01,                                        /* PT_SCAN：窗口内外栅极都扫 */
    };
    write_cmd_data(PTL, win, sizeof(win));
    send_window_bw(DTM, s_shadow, h0, h1, g0, g1);   /* 旧 */
    send_window_bw(DTM2, picData, h0, h1, g0, g1);   /* 新 */
    bw_refresh();
    EPD_W21_WriteCMD(PTOUT);
    EPD_sleep();

    /* 只把真正刷过的像素记进 shadow（窗口里左右两条） */
    for (int g = g0; g <= g1; g++) {
        int c = col_of_gate(g);
        if (c < 0) {
            continue;
        }
        for (int r = h0; r <= h1 && r < EPD_HEIGHT; r++) {
            s_shadow[r * EPD_WIDTH + c] = picData[r * EPD_WIDTH + c];
        }
    }
}

#endif

/* ================================================================
 *  公共
 * ================================================================ */
/* 物理像素 (px, py) 对应应用层图片里的下标（应用层图片 EPD_WIDTH x EPD_HEIGHT）。
 * 屏幕顺时针转了 EPD_ROTATION 度，画面就要逆时针转同样角度写进去，人看到的才是正的 */
static inline uint32_t logical_index(int px, int py)
{
#if EPD_ROTATION == 0
    return py * EPD_WIDTH + px;
#elif EPD_ROTATION == 90        /* 物理左边 = 应用层上边 */
    return px * EPD_WIDTH + (EPD_WIDTH - 1 - py);
#elif EPD_ROTATION == 180
    return (EPD_HEIGHT - 1 - py) * EPD_WIDTH + (EPD_WIDTH - 1 - px);
#else /* 270 */                 /* 物理右边 = 应用层上边 */
    return (EPD_HEIGHT - 1 - px) * EPD_WIDTH + py;
#endif
}

#if EPD_COLORS == 2
#if EPD_PANEL == EPD_PANEL_4IN0
#define EPD_GATES   EPD_4IN0_GATES      /* 每帧发的行数（芯片的全部栅极） */
/* 4 寸屏：实际第 g 行栅极在屏上从左往右第几列，换算成"栅极从右往左挨着排"时的行号，
 * 再交给 logical_index() 做旋转。不在屏上的栅极返回 -1 */
static inline int panel_row(int g)
{
    int x = (g % 2 == 0) ? g / 2 : EPD_4IN0_LEFT_COLS + g / 2;
    if (x >= EPD_PANEL_H) {
        return -1;
    }
    return EPD_PANEL_H - 1 - x;
}
#else
#define EPD_GATES   EPD_PANEL_H
static inline int panel_row(int g)
{
    return g;
}
#endif

#if EPD_PANEL == EPD_PANEL_4IN0 && EPD_4IN0_INVERT
#define BW_XOR  0xFF
#else
#define BW_XOR  0x00
#endif

static void send_window_bw(uint8_t cmd, const unsigned char *picData, int h0, int h1, int g0, int g1)
{
    const int nbytes = (h1 - h0 + 1) / 8;
    EPD_W21_WriteCMD(cmd);
    for (int g = g0; g <= g1; g++) {
        int row = panel_row(g);
        if (row < 0) {
            memset(s_row, 0xFF ^ BW_XOR, nbytes);          /* 不在屏上的栅极，发白 */
        } else {
            for (int i = 0; i < nbytes; i++) {
                uint8_t b = 0;
                for (int k = 0; k < 8; k++) {
                    uint8_t c = picData[logical_index(h0 + i * 8 + k, row)];
                    if (c == 0xFF || c == 0xFC) {           /* 白；黑白屏没有黄，按白显示 */
                        b |= 0x80 >> k;                     /* 高位在左 */
                    }
                }
                s_row[i] = b ^ BW_XOR;
            }
        }
        spi_send(1, s_row, nbytes);
    }
}

static void send_frame_bw(uint8_t cmd, const unsigned char *picData, uint8_t fill)
{
    if (picData) {
        send_window_bw(cmd, picData, 0, EPD_PANEL_W - 1, 0, EPD_GATES - 1);
        return;
    }
    memset(s_row, fill ^ BW_XOR, EPD_ROW_BYTES);
    EPD_W21_WriteCMD(cmd);
    for (int g = 0; g < EPD_GATES; g++) {
        spi_send(1, s_row, EPD_ROW_BYTES);
    }
}

#else
static uint8_t Color_get(uint8_t color)
{
    switch (color) {
    case 0xFF: return 0x01;     /* White  */
    case 0xFC: return 0x02;     /* Yellow */
    case 0xE0: return 0x03;     /* Red    */
    case 0x03: return 0x05;     /* Blue   */
    case 0x1C: return 0x06;     /* Green  */
    default:   return 0x00;     /* Black */
    }
}

static void send_frame(const unsigned char *picData, uint8_t color)
{
    if (!picData) {
        memset(s_row, color, EPD_ROW_BYTES);
    }
    EPD_W21_WriteCMD(DTM);
    for (int py = 0; py < EPD_PANEL_H; py++) {
        if (picData) {
            for (int x = 0; x < EPD_ROW_BYTES; x++) {
                uint8_t left = picData[logical_index(2 * x, py)];
                uint8_t right = picData[logical_index(2 * x + 1, py)];
                s_row[x] = (Color_get(left) << 4) | Color_get(right);
            }
        }
        spi_send(1, s_row, EPD_ROW_BYTES);
    }
}
#endif

int epd_busy_timeouts(void)
{
    return s_busy_timeouts;
}

/* 刷屏前先看屏有没有活着：复位后芯片应在 1ms 内把 BUSY 拉高（空闲）。
 * 不高的话再临时开内部上拉区分：读到 1 = BUSY 悬空（屏没接上），读到 0 = 被屏拉低（有电但芯片没起来） */
bool epd_probe(void)
{
    epd_hw_init();
    gpio_set_level(EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    int in_reset = gpio_get_level(EPD_BUSY_GPIO);
    gpio_set_level(EPD_RST_GPIO, 1);
    int idle = 0;
    for (int i = 0; i < 50 && !idle; i++) {         /* 最多等 500ms */
        vTaskDelay(pdMS_TO_TICKS(10));
        idle = gpio_get_level(EPD_BUSY_GPIO);
    }
    gpio_pullup_en(EPD_BUSY_GPIO);
    vTaskDelay(pdMS_TO_TICKS(5));
    int with_pullup = gpio_get_level(EPD_BUSY_GPIO);
    gpio_pullup_dis(EPD_BUSY_GPIO);
    ESP_LOGI(TAG, "BUSY 检查：复位中=%d，复位后=%d，加内部上拉=%d", in_reset, idle, with_pullup);

    s_dead = !idle;
    if (idle) {
        return true;
    }
    if (with_pullup) {
        ESP_LOGE(TAG, "BUSY 悬空：屏没接上（排线没插到底 / 金手指朝向反了 / 插错座子）");
    } else {
        ESP_LOGE(TAG, "BUSY 一直被拉低：屏有电但芯片没起来（查 EPD_3V3、RST(IO8)、排线方向）");
    }
    return false;
}

void epd_hw_init(void)
{
    static bool done;       /* 摄像头测试和墨水屏测试都会调，只初始化一次 */
    if (done) {
        return;
    }
    done = true;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << EPD_RST_GPIO) | (1ULL << EPD_DC_GPIO) | (1ULL << EPD_PWR_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << EPD_BUSY_GPIO,
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
    gpio_set_level(EPD_RST_GPIO, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = EPD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = EPD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_ROW_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = EPD_CS_GPIO,
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));

    s_row = heap_caps_malloc(EPD_ROW_BYTES, MALLOC_CAP_DMA);
    assert(s_row);

    /* 打开 EPD_3V3（Q7，低有效），等电源稳定 */
    gpio_set_level(EPD_PWR_EN_GPIO, EPD_PWR_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "屏 %dx%d，旋转 %d°，应用层 %dx%d，SPI %d MHz，BUSY=%d",
             EPD_PANEL_W, EPD_PANEL_H, EPD_ROTATION, EPD_WIDTH, EPD_HEIGHT,
             EPD_SPI_HZ / 1000000, gpio_get_level(EPD_BUSY_GPIO));
}

#if EPD_PANEL != EPD_PANEL_4IN0
/* 六色屏没做局刷，直接全刷 */
void epd_partial(const unsigned char *picData, int x, int y, int w, int h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    EPD_init_fast();
    PIC_display(picData);
    EPD_sleep();
}
#endif
