/**
 * @file epd.h
 * @brief 墨水屏驱动（ESP32-S31，硬件 SPI2），支持三种屏，改 EPD_PANEL 切换：
 *   EPD_PANEL_7IN3：Good Display GDEP073E01，六色 800x480，接 U1（50P），时序取自原厂 Arduino 例程
 *   EPD_PANEL_3IN6：Waveshare 3.6inch e-Paper (E)，六色 400x600，接 U1（50P），时序取自微雪 EPD_3in6e.c
 *   EPD_PANEL_4IN0：秋田微 E0400A01-AF-S，黑白 528x792，接 FPC3（24P），UC8179 命令集
 * 三种屏共用 BUSY/RST/DC/CS/SCK/MOSI 和 EPD_3V3，一次只能接一块。
 *
 * 接口沿用 GDEP073E01 例程的函数名，方便对照。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_PANEL_7IN3  1
#define EPD_PANEL_3IN6  2
#define EPD_PANEL_4IN0  3

/* ↓↓↓ 换屏改这里：EPD_PANEL_7IN3 / EPD_PANEL_3IN6 / EPD_PANEL_4IN0 ↓↓↓ */
#define EPD_PANEL       EPD_PANEL_3IN6

/* 屏幕物理分辨率 + 显示方向
 * EPD_ROTATION：屏幕实际摆放相对默认方向（排线在下方）顺时针转了多少度（0 / 90 / 180 / 270）
 *   7.3 寸：0 = 横屏 800x480（排线在下）；90 = 竖屏 480x800（排线在左）；270 = 竖屏（排线在右） */
#if EPD_PANEL == EPD_PANEL_7IN3
#define EPD_PANEL_W     800
#define EPD_PANEL_H     480
#define EPD_ROTATION    0       /* 横屏 800x480，排线在下 */
#elif EPD_PANEL == EPD_PANEL_3IN6
#define EPD_PANEL_W     400
#define EPD_PANEL_H     600
#define EPD_ROTATION    0
#elif EPD_PANEL == EPD_PANEL_4IN0
/* 规格书写 528(H)x792(V) 是竖放时的观感。芯片（UC8179，栅极最多 600 行）实际是
 * 长边 792 列（源极，竖放时从上往下）x 短边 528 行（栅极）扫描。
 * 实测：按 528x792 设分辨率时只亮了上面 528/792，画面错乱。
 * 竖放、排线在下：EPD_ROTATION 90，应用层 528x792 */
#define EPD_PANEL_W     792
#define EPD_PANEL_H     528
#define EPD_ROTATION    90
#define EPD_COLORS      2       /* 黑白 */
/* 面板设置 PSR：0x1F = 黑白 OTP 波形。画面左右镜像改 0x17（反转栅极扫描），
 * 上下颠倒改 0x1B（反转源极扫描），两个都反改 0x13 */
#define EPD_4IN0_PSR    0x1F
/* 实测栅极（短边）接法：偶数栅极 G0~G598 从左往右排 300 列，奇数栅极 G1~G455 接着排右边 228 列，
 * 其余奇数栅极不在屏上。所以芯片要按 600 行（全部栅极）刷，只按 528 行刷时中间 36 列不动 */
#define EPD_4IN0_GATES      600
#define EPD_4IN0_LEFT_COLS  300
/* 实测数据 1 显示黑、0 显示白，和程序约定（1 白 0 黑）相反，发送时取反。黑白颠倒就改成 0 */
#define EPD_4IN0_INVERT     1
/* 局刷波形：0 = 用 OTP 里的全刷波形（窗口内会闪一下，约 4 秒）。
 * 秋田微如果告诉你 OTP 里有快刷/局刷波形、要用"强制温度"选（CCSET 0xE0=0x02 + TSSET 0xE5=温度），
 * 把那个温度值填这里（例如 0x5A），局刷时会切到那套波形。不要乱填，问厂家要值 */
#define EPD_4IN0_PARTIAL_TEMP   0
#else
#error "EPD_PANEL 设置错误"
#endif

#ifndef EPD_COLORS
#define EPD_COLORS      6       /* 六色：黑 白 黄 红 蓝 绿 */
#endif

/* 应用层看到的分辨率（图片、画图都按这个尺寸） */
#if EPD_ROTATION == 0 || EPD_ROTATION == 180
#define EPD_WIDTH       EPD_PANEL_W
#define EPD_HEIGHT      EPD_PANEL_H
#elif EPD_ROTATION == 90 || EPD_ROTATION == 270
#define EPD_WIDTH       EPD_PANEL_H
#define EPD_HEIGHT      EPD_PANEL_W
#else
#error "EPD_ROTATION 只能是 0 / 90 / 180 / 270"
#endif

/* 8bit 颜色：一个字节两个像素，高 4 位在左 */
#define EPD_BLACK       0x00
#define EPD_WHITE       0x11
#define EPD_YELLOW      0x22
#define EPD_RED         0x33
#define EPD_BLUE        0x55
#define EPD_GREEN       0x66

/**
 * @brief 初始化 GPIO、SPI，并打开 EPD_3V3 电源。其他函数调用前先调一次。
 */
void epd_hw_init(void);

/** @brief 累计 BUSY 超时次数（屏没响应时会增加），用来判断刷屏是否成功 */
int epd_busy_timeouts(void);

/**
 * @brief 复位屏并检查 BUSY 能否变成空闲（高），判断屏有没有接好、芯片有没有起来。
 *        失败时日志里会写是"悬空（没接上）"还是"被拉低（芯片没起来）"。刷屏前先调。
 */
bool epd_probe(void);

void EPD_init(void);        /* 普通全刷初始化，每次全刷前都要调用 */
void EPD_init_fast(void);   /* 快速全刷初始化（3.6 寸没有快刷，等同 EPD_init） */
void EPD_sleep(void);       /* 刷完必须调用，否则影响屏幕寿命 */

/**
 * @brief 全屏显示一张图（含刷新，阻塞直到刷完）
 * @param picData EPD_WIDTH x EPD_HEIGHT 字节，每字节一个像素：
 *                0x00 黑 / 0xFF 白 / 0xFC 黄 / 0xE0 红 / 0x03 蓝 / 0x1C 绿，其他按黑处理
 *                黑白屏：0xFF 和 0xFC 显示白，其余显示黑
 */
void PIC_display(const unsigned char *picData);
void PIC_display_Clear(void);

/**
 * @brief 局部刷新：只刷 (x, y, w, h) 这个矩形（应用层坐标），其它地方不动。内部自己初始化和休眠。
 * @param picData 整屏新画面（EPD_WIDTH x EPD_HEIGHT，格式同 PIC_display），矩形外的内容应和屏上一致
 * @note  目前只有 4 寸黑白屏真正局刷，六色屏退化成全刷；没全刷过时第一次会先全刷。
 *        4 寸屏栅极奇偶分在左右两边，矩形会扩成左右各一条窗口一起刷（内容不变的部分刷完还是原样）。
 */
void epd_partial(const unsigned char *picData, int x, int y, int w, int h);

/**
 * @brief 全屏刷单色（原例程的 EPD_Display_White/Black/Yellow/blue/Green/red）
 * @param color EPD_WHITE / EPD_BLACK / EPD_YELLOW / EPD_BLUE / EPD_GREEN / EPD_RED
 *              黑白屏只有 EPD_WHITE 显示白，其余都显示黑
 */
void EPD_Display_Color(uint8_t color);

#ifdef __cplusplus
}
#endif
