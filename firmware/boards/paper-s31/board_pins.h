/**
 * @file board_pins.h
 * @brief 墨之坊 paper（墨水屏研究板 V2）板级引脚定义（纯宏头文件，无函数）
 *
 * 主控：U6 ESP32-S31NRV16
 * 原理图：SCH_墨水屏paperV2_2026-09-29_01~05.png
 *   _04 主控 / _03 墨水屏 + SD 卡 / _02 Codec + 功放 + MIC / _05 电源 / _01 RTC + IMU
 *
 * === 使用示例 ===
 * @code{.c}
 * #include "board_pins.h"
 * gpio_set_level(EPD_PWR_EN_GPIO, EPD_PWR_ON_LEVEL);
 * @endcode
 *
 * === 原理图上需要确认的问题 ===
 * 1. POWER_CTRL（Q5 栅极，MCU_3V3 -> VCC_3V3）没有接任何 GPIO，R35 上拉使其默认关断。
 *    VCC_3V3 给 I2C 上拉、GPIO36 strap 上拉、SD/墨水屏电源开关、摄像头 FPC、U29 供电。
 * 2. GPIO36 = I2S_MCLK，同时是 Flash 电压 strap（1=3.3V，0=1.8V），上电采样时必须为高。
 * 3. GPIO44 = EPD_PWR_EN，同时引到扩展口 U29 第 5 脚。
 * 4. GPIO54~57（MTDO/MTCK/MTDI/MTMS）被摄像头占用，不能用管脚 JTAG。
 */
#pragma once

#include "hal/gpio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  共用 I2C 总线
 *
 *  R40/R49 2.2kΩ 上拉到 VCC_3V3。
 *  挂载：ES8389、PCF85063、SC7A20、BQ27220、触摸（FPC2）、
 *        摄像头 SCCB（U30 13/14 脚）、扩展口 U29 3/4 脚。
 * ================================================================ */
#define I2C_SDA_GPIO            GPIO_NUM_0
#define I2C_SCL_GPIO            GPIO_NUM_1

/* 7 位地址 */
#define ES8389_I2C_ADDR         0x10    /* 原理图标 0x20 为 8 位写地址；esp_codec_dev 的 ES8389_CODEC_DEFAULT_ADDR 即 0x20 */
#define PCF85063_I2C_ADDR       0x51    /* RTC */
#define BQ27220_I2C_ADDR        0x55    /* 电量计 */
#define SC7A20_I2C_ADDR         0x19    /* SDO 只接 TP14，可能为 0x18，上电扫描确认 */

/* ================================================================
 *  墨水屏 SPI
 *
 *  U1（50P，3.6" E6 / 7.3" E6）与 FPC3（24P，3.7" / 4" 4 阶灰）共用同一组网络。
 *  U1 的 BS1 固定接地；BS0 由跳线 H1 选择：接 GND = 4 线 SPI（带 DC），接 3V3 = 3 线 SPI。
 * ================================================================ */
#define EPD_BUSY_GPIO           GPIO_NUM_7
#define EPD_RST_GPIO            GPIO_NUM_8
#define EPD_DC_GPIO             GPIO_NUM_9
#define EPD_CS_GPIO             GPIO_NUM_10
#define EPD_MOSI_GPIO           GPIO_NUM_11   /* EPD_DIN */
#define EPD_SCK_GPIO            GPIO_NUM_12

/* EPD_3V3 电源：Q7 AO3401A，R13 上拉到 VCC_3V3，默认关断 */
#define EPD_PWR_EN_GPIO         GPIO_NUM_44
#define EPD_PWR_ON_LEVEL        0
#define EPD_PWR_OFF_LEVEL       1

/* ================================================================
 *  触摸（FPC2 6P：1 GND / 2 INT / 3 RST / 4 EPD_3V3 / 5 SCL / 6 SDA）
 * ================================================================ */
#define TOUCH_I2C_SDA_GPIO      I2C_SDA_GPIO
#define TOUCH_I2C_SCL_GPIO      I2C_SCL_GPIO
#define TOUCH_RST_GPIO          GPIO_NUM_4
#define TOUCH_INT_GPIO          GPIO_NUM_5

/* ================================================================
 *  音频：ES8389 Codec + NS4150B 功放
 *
 *  原理图网络名 I2S_DI / I2S_DO 是按 Codec 视角命名的，这里改成按 ESP 视角：
 *    I2S_DOUT = ESP 输出 -> ES8389 DSDIN（播放），网络名 I2S_DI
 *    I2S_DIN  = ES8389 ASDOUT -> ESP 输入（录音），网络名 I2S_DO
 * ================================================================ */
#define I2S_MCLK_GPIO           GPIO_NUM_36   /* 兼 Flash 电压 strap，见文件头 */
#define I2S_BCK_GPIO            GPIO_NUM_16
#define I2S_WS_GPIO             GPIO_NUM_17
#define I2S_DOUT_GPIO           GPIO_NUM_18
#define I2S_DIN_GPIO            GPIO_NUM_37

/* NS4150B CTRL，R6 10kΩ 下拉，默认关闭 */
#define PA_CTRL_GPIO            GPIO_NUM_19
#define PA_ON_LEVEL             1

/* ================================================================
 *  摄像头 DVP 8 位（经 U30 32P FPC 接子板）
 * ================================================================ */
#define CAM_SCCB_SDA_GPIO       I2C_SDA_GPIO  /* FPC 13 */
#define CAM_SCCB_SCL_GPIO       I2C_SCL_GPIO  /* FPC 14 */
/* CAM_PWR_EN：Q5（AO3401A P-MOS）栅极，R35 10k 上拉到 VCC_3V3，低电平打开 CAM_3V3。
 * 没有软启动，直接拉低时 CAM_3V3 电容冲击电流会把 VCC_3V3 拉垮、整板复位（实测）；
 * 软件里用脉冲软启动临时规避（test_cam.c），硬件建议栅极加 RC 软启动。 */
#define CAM_PWR_EN_GPIO         GPIO_NUM_2    /* FPC 7 */
#define CAM_PWR_ON_LEVEL        0
#define CAM_PWR_OFF_LEVEL       1
#define CAM_GM_FK_GPIO          GPIO_NUM_38   /* FPC 15 */
#define CAM_XCLK_GPIO           GPIO_NUM_55   /* FPC 16，XMCLK */
#define CAM_PCLK_GPIO           GPIO_NUM_54   /* FPC 18 */
#define CAM_VSYNC_GPIO          GPIO_NUM_56   /* FPC 19 */
#define CAM_HREF_GPIO           GPIO_NUM_57   /* FPC 20 */
#define CAM_D0_GPIO             GPIO_NUM_46   /* Y2，FPC 22 */
#define CAM_D1_GPIO             GPIO_NUM_47   /* Y3，FPC 23 */
#define CAM_D2_GPIO             GPIO_NUM_48   /* Y4，FPC 24 */
#define CAM_D3_GPIO             GPIO_NUM_49   /* Y5，FPC 25 */
#define CAM_D4_GPIO             GPIO_NUM_50   /* Y6，FPC 27 */
#define CAM_D5_GPIO             GPIO_NUM_51   /* Y7，FPC 28 */
#define CAM_D6_GPIO             GPIO_NUM_52   /* Y8，FPC 29 */
#define CAM_D7_GPIO             GPIO_NUM_53   /* Y9，FPC 30 */
#define CAM_PWDN_GPIO           GPIO_NUM_NC
#define CAM_RESET_GPIO          GPIO_NUM_NC

/* ================================================================
 *  按键 / LED 子板（同一 U30 FPC）
 * ================================================================ */
#define TOP_LED_EN_GPIO         GPIO_NUM_3    /* FPC 8 */
#define KEY1_GPIO               GPIO_NUM_13   /* FPC 9 */
#define KEY2_GPIO               GPIO_NUM_14   /* FPC 10 */
#define KEY3_GPIO               GPIO_NUM_15   /* FPC 11 */

/* ================================================================
 *  microSD 卡槽 —— SDMMC 4-bit
 *
 *  原理图只标了芯片管脚号（D0~D3 = 27/28/29/31，CLK = 32，CMD = 33），
 *  GPIO 号按管脚顺序推出，与官方 ESP32-S31-Korvo BSP 一致。
 *  数据线 10kΩ 上拉到 SD_3V3。
 * ================================================================ */
#define SDMMC_D0_GPIO           GPIO_NUM_20
#define SDMMC_D1_GPIO           GPIO_NUM_21
#define SDMMC_D2_GPIO           GPIO_NUM_22
#define SDMMC_D3_GPIO           GPIO_NUM_23
#define SDMMC_CLK_GPIO          GPIO_NUM_24
#define SDMMC_CMD_GPIO          GPIO_NUM_25

/* SD_3V3 电源：Q8 AO3401A，R80 下拉，默认导通，拉高断电 */
#define SD_CTRL_GPIO            GPIO_NUM_35
#define SD_PWR_ON_LEVEL         0
#define SD_PWR_OFF_LEVEL        1

/* ================================================================
 *  中断 / 电源 / 其他
 * ================================================================ */
#define RTC_INT_GPIO            GPIO_NUM_39   /* TIME_INT，PCF85063 INT#，R47 10kΩ 上拉，低有效 */
#define IMU_INT_GPIO            GPIO_NUM_43   /* SC7A20 INT1，经 R15 0Ω */
#define POWER_SWITCH_GPIO       GPIO_NUM_6    /* 电源键 SW4 状态检测 */

/* 震动马达：R41 1kΩ -> SS8050 -> M1，R44 10kΩ 下拉 */
#define MOTOR_GPIO              GPIO_NUM_42
#define MOTOR_ON_LEVEL          1

/* Type-C D-/D+ 经 R58/R59 0Ω 接到 GPIO33/34；芯片原生 USB_DM/DP（45/44 脚）只接 TP7/TP8 */
#define USB_DM_GPIO             GPIO_NUM_33
#define USB_DP_GPIO             GPIO_NUM_34

#define UART0_TX_GPIO           GPIO_NUM_58   /* 串 R16 499Ω */
#define UART0_RX_GPIO           GPIO_NUM_59   /* 串 R36 0Ω */

#define BOOT_GPIO               GPIO_NUM_61   /* SW2，R9 10kΩ 上拉，按下为低 */
#define TP6_GPIO                GPIO_NUM_60   /* 仅测试点 */

/* ================================================================
 *  扩展口 U29（HC-PM254 1x6）
 *  1 GND / 2 VCC_3V3 / 3 GPIO0(SDA) / 4 GPIO1(SCL) / 5 GPIO44 / 6 GPIO45
 * ================================================================ */
#define EXT_IO5_GPIO            GPIO_NUM_44   /* 与 EPD_PWR_EN 共用！ */
#define EXT_IO6_GPIO            GPIO_NUM_45

/* 未连接：GPIO40 */

#ifdef __cplusplus
}
#endif
