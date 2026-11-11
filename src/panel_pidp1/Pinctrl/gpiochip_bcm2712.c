/*
 *  GPIO_CHIP_T implementations for the BCM2712 SoC (Pi 5), that splits
 *  GPIO handling across two device-tree nodes, a "brcmstb,gpio"-style level/direction block
 *  (bcm2712GpioInterface) and a separate pinctrl block for function-select/pull
 *  (bcm2712PinctrlInterface), registered once per C0/D0 stepping x AON/main-bank combination).
 *  The two sides share per-SoC-instance state (BCM2712_INST_T, in bcm2712Instances[]) matched
 *  up by AON-ness as each side probes.
 *  All functions/types here are file-local and have been renamed to match the wje coding conventions.
 *  However, externally visible names have been left in the orginal form
 *
 * 04-Jul-2026 wje initial rework
*/

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gpiochip.h"
#include "util.h"

#define ARRAY_SIZE(_a) (sizeof(_a) / sizeof(_a[0]))

/* 2712 definitions */

#define BCM2712_GIO_DATA       0x04
#define BCM2712_GIO_IODIR      0x08

#define BCM2712_PAD_PULL_OFF   0
#define BCM2712_PAD_PULL_DOWN  1
#define BCM2712_PAD_PULL_UP    2

#define BCM2712_MAX_INSTANCES  2
#define BCM2712_FSEL_COUNT     9

#define FLAGS_AON              1
#define FLAGS_C0               2
#define FLAGS_D0               4
#define FLAGS_GPIO             8
#define FLAGS_PINCTRL          16

// Per-SoC-instance state shared between a BCM2712 "gpio" chip registration and its matching
// "pinctrl" chip registration.
typedef struct
{
    volatile uint32_t *gpioBaseP;
    volatile uint32_t *pinmuxBaseP;
    unsigned padOffset;
    uint32_t *bankWidthsP;
    unsigned flags;
    unsigned numGpios;
    unsigned numBanks;
} BCM2712_INST_T;

static unsigned numInstances;
static BCM2712_INST_T bcm2712Instances[BCM2712_MAX_INSTANCES] = { 0 };
static unsigned sharedFlags;

static const char *bcm2712C0GpioAltNames[][BCM2712_FSEL_COUNT - 1] =
{
    { "BSC_M3_SDA"             , "VC_SDA0"          , "GPCLK0"           , "ENET0_LINK"        , "VC_PWM1_0"             , "VC_SPI0_CE1_N"         , "IR_IN"          , }, // 0
    { "BSC_M3_SCL"             , "VC_SCL0"          , "GPCLK1"           , "ENET0_ACTIVITY"    , "VC_PWM1_1"             , "SR_EDM_SENSE"          , "VC_SPI0_CE0_N"  , "VC_TXD3"       , }, // 1
    { "PDM_CLK"                , "I2S_CLK0_IN"      , "GPCLK2"           , "VC_SPI4_CE1_N"     , "PKT_CLK0"              , "VC_SPI0_MISO"          , "VC_RXD3"        , }, // 2
    { "PDM_DATA0"              , "I2S_LR0_IN"       , "VC_SPI4_CE0_N"    , "PKT_SYNC0"         , "VC_SPI0_MOSI"          , "VC_CTS3"               , }, // 3
    { "PDM_DATA1"              , "I2S_DATA0_IN"     , "ARM_RTCK"         , "VC_SPI4_MISO"      , "PKT_DATA0"             , "VC_SPI0_SCLK"          , "VC_RTS3"        , }, // 4
    { "PDM_DATA2"              , "VC_SCL3"          , "ARM_TRST"         , "SD_CARD_LED_E"     , "VC_SPI4_MOSI"          , "PKT_CLK1"              , "VC_PCM_CLK"     , "VC_SDA5"       , }, // 5
    { "PDM_DATA3"              , "VC_SDA3"          , "ARM_TCK"          , "SD_CARD_WPROT_E"   , "VC_SPI4_SCLK"          , "PKT_SYNC1"             , "VC_PCM_FS"      , "VC_SCL5"       , }, // 6
    { "I2S_CLK0_OUT"           , "SPDIF_OUT"        , "ARM_TDI"          , "SD_CARD_PRES_E"    , "VC_SDA3"               , "ENET0_RGMII_START_STOP", "VC_PCM_DIN"     , "VC_SPI4_CE1_N" , }, // 7
    { "I2S_LR0_OUT"            , "AUD_FS_CLK0"      , "ARM_TMS"          , "SD_CARD_VOLT_E"    , "VC_SCL3"               , "ENET0_MII_TX_ERR"      , "VC_PCM_DOUT"    , "VC_SPI4_CE0_N" , }, // 8
    { "I2S_DATA0_OUT"          , "AUD_FS_CLK0"      , "ARM_TDO"          , "SD_CARD_PWR0_E"    , "ENET0_MII_RX_ERR"      , "SD_CARD_VOLT_C"        , "VC_SPI4_SCLK"   , }, // 9
    { "BSC_M3_SCL"             , "MTSIF_DATA4_ALT1" , "I2S_CLK0_IN"      , "I2S_CLK0_OUT"      , "VC_SPI5_CE1_N"         , "ENET0_MII_CRS"         , "SD_CARD_PWR0_C" , "VC_SPI4_MOSI"  , }, // 10
    { "BSC_M3_SDA"             , "MTSIF_DATA5_ALT1" , "I2S_LR0_IN"       , "I2S_LR0_OUT"       , "VC_SPI5_CE0_N"         , "ENET0_MII_COL"         , "SD_CARD_PRES_C" , "VC_SPI4_MISO"  , }, // 11
    { "SPI_S_SS0B"             , "MTSIF_DATA6_ALT1" , "I2S_DATA0_IN"     , "I2S_DATA0_OUT"     , "VC_SPI5_MISO"          , "VC_I2CSL_MOSI"         , "SD0_CLK"        , "SD_CARD_VOLT_D", }, // 12
    { "SPI_S_MISO"             , "MTSIF_DATA7_ALT1" , "I2S_DATA1_OUT"    , "USB_VBUS_PRESENT"  , "VC_SPI5_MOSI"          , "VC_I2CSL_CE_N"         , "SD0_CMD"        , "SD_CARD_PWR0_D", }, // 13
    { "SPI_S_MOSI_OR_BSC_S_SDA", "VC_I2CSL_SCL_SCLK", "ENET0_RGMII_RX_OK", "ARM_TCK"           , "VC_SPI5_SCLK"          , "VC_PWM0_0"             , "VC_SDA4"        , "SD_CARD_PRES_D", }, // 14
    { "SPI_S_SCK_OR_BSC_S_SCL" , "VC_I2CSL_SDA_MISO", "VC_SPI3_CE1_N"    , "ARM_TMS"           , "VC_PWM0_1"             , "VC_SCL4"               , "GPCLK0"         , }, // 15
    { "SD_CARD_PRES_B"         , "I2S_CLK0_OUT"     , "VC_SPI3_CE0_N"    , "I2S_CLK0_IN"       , "SD0_DAT0"              , "ENET0_RGMII_MDIO"      , "GPCLK1"         , }, // 16
    { "SD_CARD_WPROT_B"        , "I2S_LR0_OUT"      , "VC_SPI3_MISO"     , "I2S_LR0_IN"        , "EXT_SC_CLK"            , "SD0_DAT1"              , "ENET0_RGMII_MDC", "GPCLK2"        , }, // 17
    { "SD_CARD_LED_B"          , "I2S_DATA0_OUT"    , "VC_SPI3_MOSI"     , "I2S_DATA0_IN"      , "SD0_DAT2"              , "ENET0_RGMII_IRQ"       , "VC_PWM1_0"      , }, // 18
    { "SD_CARD_VOLT_B"         , "USB_PWRFLT"       , "VC_SPI3_SCLK"     , "PKT_DATA1"         , "SPDIF_OUT"             , "SD0_DAT3"              , "IR_IN"          , "VC_PWM1_1"     , }, // 19
    { "SD_CARD_PWR0_B"         , "UUI_TXD"          , "VC_TXD0"          , "ARM_TMS"           , "UART_TXD_2"            , "USB_PWRON"             , "VC_PCM_CLK"     , "VC_TXD4"       , }, // 20
    { "USB_PWRFLT"             , "UUI_RXD"          , "VC_RXD0"          , "ARM_TCK"           , "UART_RXD_2"            , "SD_CARD_VOLT_B"        , "VC_PCM_FS"      , "VC_RXD4"       , }, // 21
    { "USB_PWRON"              , "ENET0_LINK"       , "VC_CTS0"          , "MTSIF_ATS_RST"     , "UART_RTS_2"            , "USB_VBUS_PRESENT"      , "VC_PCM_DIN"     , "VC_SDA5"       , }, // 22
    { "USB_VBUS_PRESENT"       , "ENET0_ACTIVITY"   , "VC_RTS0"          , "MTSIF_ATS_INC"     , "UART_CTS_2"            , "I2S_DATA2_OUT"         , "VC_PCM_DOUT"    , "VC_SCL5"       , }, // 23
    { "MTSIF_ATS_RST"          , "PKT_CLK0"         , "UART_RTS_0"       , "ENET0_RGMII_RX_CLK", "ENET0_RGMII_START_STOP", "VC_SDA4"               , "VC_TXD3"        , }, // 24
    { "MTSIF_ATS_INC"          , "PKT_SYNC0"        , "SC0_CLK"          , "UART_CTS_0"        , "ENET0_RGMII_RX_EN_CTL" , "ENET0_RGMII_RX_OK"     , "VC_SCL4"        , "VC_RXD3"       , }, // 25
    { "MTSIF_DATA1"            , "PKT_DATA0"        , "SC0_IO"           , "UART_TXD_0"        , "ENET0_RGMII_RXD_00"    , "VC_TXD4"               , "VC_SPI5_CE0_N"  , }, // 26
    { "MTSIF_DATA2"            , "PKT_CLK1"         , "SC0_AUX1"         , "UART_RXD_0"        , "ENET0_RGMII_RXD_01"    , "VC_RXD4"               , "VC_SPI5_SCLK"   , }, // 27
    { "MTSIF_CLK"              , "PKT_SYNC1"        , "SC0_AUX2"         , "ENET0_RGMII_RXD_02", "VC_CTS4"               , "VC_SPI5_MOSI"          , }, // 28
    { "MTSIF_DATA0"            , "PKT_DATA1"        , "SC0_PRES"         , "ENET0_RGMII_RXD_03", "VC_RTS4"               , "VC_SPI5_MISO"          , }, // 29
    { "MTSIF_SYNC"             , "PKT_CLK2"         , "SC0_RST"          , "SD2_CLK"           , "ENET0_RGMII_TX_CLK"    , "GPCLK0"                , "VC_PWM0_0"      , }, // 30
    { "MTSIF_DATA3"            , "PKT_SYNC2"        , "SC0_VCC"          , "SD2_CMD"           , "ENET0_RGMII_TX_EN_CTL" , "VC_SPI3_CE1_N"         , "VC_PWM0_1"      , }, // 31
    { "MTSIF_DATA4"            , "PKT_DATA2"        , "SC0_VPP"          , "SD2_DAT0"          , "ENET0_RGMII_TXD_00"    , "VC_SPI3_CE0_N"         , "VC_TXD3"        , }, // 32
    { "MTSIF_DATA5"            , "PKT_CLK3"         , "SD2_DAT1"         , "ENET0_RGMII_TXD_01", "VC_SPI3_SCLK"          , "VC_RXD3"               , }, // 33
    { "MTSIF_DATA6"            , "PKT_SYNC3"        , "EXT_SC_CLK"       , "SD2_DAT2"          , "ENET0_RGMII_TXD_02"    , "VC_SPI3_MOSI"          , "VC_SDA5"        , }, // 34
    { "MTSIF_DATA7"            , "PKT_DATA3"        , "SD2_DAT3"         , "ENET0_RGMII_TXD_03", "VC_SPI3_MISO"          , "VC_SCL5"               , }, // 35
    { "SD0_CLK"                , "MTSIF_ATS_RST"    , "SC0_RST"          , "I2S_DATA1_IN"      , "VC_TXD3"               , "VC_TXD2"               , }, // 36
    { "SD0_CMD"                , "MTSIF_ATS_INC"    , "SC0_VCC"          , "VC_SPI0_CE1_N"     , "I2S_DATA2_IN"          , "VC_RXD3"               , "VC_RXD2"        , }, // 37
    { "SD0_DAT0"               , "MTSIF_DATA4_ALT"  , "SC0_VPP"          , "VC_SPI0_CE0_N"     , "I2S_DATA3_IN"          , "VC_CTS3"               , "VC_RTS2"        , }, // 38
    { "SD0_DAT1"               , "MTSIF_DATA5_ALT"  , "SC0_CLK"          , "VC_SPI0_MISO"      , "VC_RTS3"               , "VC_CTS2"               , }, // 39
    { "SD0_DAT2"               , "MTSIF_DATA6_ALT"  , "SC0_IO"           , "VC_SPI0_MOSI"      , "BSC_M3_SDA"            , }, // 40
    { "SD0_DAT3"               , "MTSIF_DATA7_ALT"  , "SC0_PRES"         , "VC_SPI0_SCLK"      , "BSC_M3_SCL"            , }, // 41
    { "VC_SPI0_CE1_N"          , "MTSIF_CLK_ALT"    , "VC_SDA0"          , "SD_CARD_PRES_A"    , "MTSIF_CLK_ALT1"        , "ARM_TRST"              , "PDM_CLK"        , "SPI_M_SS1B"    , }, // 42
    { "VC_SPI0_CE0_N"          , "MTSIF_SYNC_ALT"   , "VC_SCL0"          , "SD_CARD_PWR0_A"    , "MTSIF_SYNC_ALT1"       , "ARM_RTCK"              , "PDM_DATA0"      , "SPI_M_SS0B"    , }, // 43
    { "VC_SPI0_MISO"           , "MTSIF_DATA0_ALT"  , "ENET0_LINK"       , "SD_CARD_LED_A"     , "MTSIF_DATA0_ALT1"      , "ARM_TDO"               , "PDM_DATA1"      , "SPI_M_MISO"    , }, // 44
    { "VC_SPI0_MOSI"           , "MTSIF_DATA1_ALT"  , "ENET0_ACTIVITY"   , "SD_CARD_VOLT_A"    , "MTSIF_DATA1_ALT1"      , "ARM_TCK"               , "PDM_DATA2"      , "SPI_M_MOSI"    , }, // 45
    { "VC_SPI0_SCLK"           , "MTSIF_DATA2_ALT"  , "SD_CARD_WPROT_A"  , "MTSIF_DATA2_ALT1"  , "ARM_TDI"               , "PDM_DATA3"             , "SPI_M_SCK"      , }, // 46
    { "ENET0_ACTIVITY"         , "MTSIF_DATA3_ALT"  , "I2S_DATA3_OUT"    , "MTSIF_DATA3_ALT1"  , "ARM_TMS"               , }, // 47
    { "SC0_RST"                , "USB_PWRFLT"       , "SPDIF_OUT"        , "MTSIF_ATS_RST"     , }, // 48
    { "SC0_VCC"                , "USB_PWRON"        , "AUD_FS_CLK0"      , "MTSIF_ATS_INC"     , }, // 49
    { "SC0_VPP"                , "USB_VBUS_PRESENT" , "SC0_AUX1"         , }, // 50
    { "SC0_CLK"                , "ENET0_LINK"       , "SC0_AUX2"         , "SR_EDM_SENSE"      , }, // 51
    { "SC0_IO"                 , "ENET0_ACTIVITY"   , "VC_PWM1_1"        , }, // 52
    { "SC0_PRES"               , "ENET0_RGMII_RX_OK", "EXT_SC_CLK"       , }, // 53
};

static const char *bcm2712D0GpioAltNames[][BCM2712_FSEL_COUNT - 1] =
{
    { "" }, // 0
    { "VC_SCL0"                , "USB_PWRFLT"       , "GPCLK0"           , "SD_CARD_LED_E"    , "VC_SPI3_CE1_N"          , "SR_EDM_SENSE"          , "VC_SPI0_CE0_N"    , "VC_TXD0"       , }, // 1
    { "VC_SDA0"                , "USB_PWRON"        , "GPCLK1"           , "SD_CARD_WPROT_E"  , "VC_SPI3_CE0_N"          , "CLK_OBSERVE"           , "VC_SPI0_MISO"     , "VC_RXD0"       , }, // 2
    { "VC_SCL3"                , "USB_VBUS_PRESENT" , "GPCLK2"           , "SD_CARD_PRES_E"   , "VC_SPI3_MISO"           , "VC_SPI0_MOSI"          , "VC_CTS0"          , }, // 3
    { "VC_SDA3"                , "VC_PWM1_1"        , "VC_SPI3_CE0_N"    , "SD_CARD_VOLT_E"   , "VC_SPI3_MOSI"           , "VC_SPI0_SCLK"          , "VC_RTS0"          , }, // 4
    { "" }, // 5
    { "" }, // 6
    { "" }, // 7
    { "" }, // 8
    { "" }, // 9
    { "BSC_M3_SCL"             , "VC_PWM1_0"        , "VC_SPI3_CE1_N"    , "SD_CARD_PWR0_E"    , "VC_SPI3_SCLK"          , "GPCLK0"                , }, // 10
    { "BSC_M3_SDA"             , "VC_SPI3_MISO"     , "CLK_OBSERVE"      , "SD_CARD_PRES_C"    , "GPCLK1"                , }, // 11
    { "SPI_S_SS0B"             , "VC_SPI3_MOSI"     , "SD_CARD_PWR0_C"   , "SD_CARD_VOLT_D"    , }, // 12
    { "SPI_S_MISO"             , "VC_SPI3_SCLK"     , "SD_CARD_PRES_C"   , "SD_CARD_PWR0_D"    , }, // 13
    { "SPI_S_MOSI_OR_BSC_S_SDA", "UUI_TXD"          , "ARM_TCK"          , "VC_PWM0_0"         , "VC_SDA0"               , "SD_CARD_PRES_D"        , }, // 14
    { "SPI_S_SCK_OR_BSC_S_SCL" , "UUI_RXD"          , "ARM_TMS"          , "VC_PWM0_1"         , "VC_SCL0"               , "GPCLK0"                , }, // 15
    { "" }, // 16
    { "" }, // 17
    { "SD_CARD_PRES_F"         , "VC_PWM1_0"        , }, // 18
    { "SD_CARD_PWR0_F"         , "USB_PWRFLT"       , "VC_PWM1_1"        , }, // 19
    { "VC_SDA3"                , "UUI_TXD"          , "VC_TXD0"          , "ARM_TMS"           , "VC_TXD2"               , }, // 20
    { "VC_SCL3"                , "UUI_RXD"          , "VC_RXD0"          , "ARM_TCK"           , "VC_RXD2"               , }, // 21
    { "SD_CARD_PRES_F"         , "VC_CTS0"          , "VC_SDA3"          , }, // 22
    { "VC_RTS0"                , "VC_SCL3"          , }, // 23
    { "SD_CARD_PRES_B"         , "VC_SPI0_CE1_N"    , "ARM_TRST"         , "UART_RTS_0"        , "USB_PWRFLT"            , "VC_RTS2"               , "VC_TXD0"        , }, // 24
    { "SD_CARD_WPROT_B"        , "VC_SPI0_CE0_N"    , "ARM_TCK"          , "UART_CTS_0"        , "USB_PWRON"             , "VC_CTS2"               , "VC_RXD0"        , }, // 25
    { "SD_CARD_LED_B"          , "VC_SPI0_MISO"     , "ARM_TDI"          , "UART_TXD_0"        , "USB_VBUS_PRESENT"      , "VC_TXD2"               , "VC_SPI0_CE0_N"  , }, // 26
    { "SD_CARD_VOLT_B"         , "VC_SPI0_MOSI"     , "ARM_TMS"          , "UART_RXD_0"        , "VC_RXD2"               , "VC_SPI0_SCLK"          , }, // 27
    { "SD_CARD_PWR0_B"         , "VC_SPI0_SCLK"     , "ARM_TDO"          , "VC_SDA0"           , "VC_SPI0_MOSI"          , }, // 28
    { "ARM_RTCK"               , "VC_SCL0"          , "VC_SPI0_MISO"     , }, // 29
    { "SD2_CLK"                , "GPCLK0"           , "VC_PWM0_0"        , }, // 30
    { "SD2_CMD"                , "VC_SPI3_CE1_N"    , "VC_PWM0_1"        , }, // 31
    { "SD2_DAT0"               , "VC_SPI3_CE0_N"    , "VC_TXD3"          , }, // 32
    { "SD2_DAT1"               , "VC_SPI3_SCLK"     , "VC_RXD3"          , }, // 33
    { "SD2_DAT2"               , "VC_SPI3_MOSI"     , "VC_SDA5"          , }, // 34
    { "SD2_DAT3"               , "VC_SPI3_MISO"     , "VC_SCL5"          , }, // 35
};

static const char *bcm2712C0AonGpioAltNames[][BCM2712_FSEL_COUNT - 1] =
{
    { "IR_IN"           , "VC_SPI0_CE1_N"     , "VC_TXD3"          , "VC_SDA3"           , "TE0"          , "VC_SDA0"         , }, // 0
    { "VC_PWM0_0"       , "VC_SPI0_CE0_N"     , "VC_RXD3"          , "VC_SCL3"           , "TE1"          , "AON_PWM0"        , "VC_SCL0"       , "VC_PWM1_0"     , }, // 1
    { "VC_PWM0_1"       , "VC_SPI0_MISO"      , "VC_CTS3"          , "CTL_HDMI_5V"       , "FL0"          , "AON_PWM1"        , "IR_IN"         , "VC_PWM1_1"     , }, // 2
    { "IR_IN"           , "VC_SPI0_MOSI"      , "VC_RTS3"          , "AON_FP_4SEC_RESETB", "FL1"          , "SD_CARD_VOLT_G"  , "AON_GPCLK"     , }, // 3
    { "GPCLK0"          , "VC_SPI0_SCLK"      , "VC_I2CSL_SCL_SCLK", "AON_GPCLK"         , "PM_LED_OUT"   , "AON_PWM0"        , "SD_CARD_PWR0_G", "VC_PWM0_0"     , }, // 4
    { "GPCLK1"          , "IR_IN"             , "VC_I2CSL_SDA_MISO", "CLK_OBSERVE"       , "AON_PWM1"     , "SD_CARD_PRES_G"  , "VC_PWM0_1"     , }, // 5
    { "UART_TXD_1"      , "VC_TXD4"           , "GPCLK2"           , "CTL_HDMI_5V"       , "VC_TXD0"      , "VC_SPI3_CE0_N"   , }, // 6
    { "UART_RXD_1"      , "VC_RXD4"           , "GPCLK0"           , "AON_PWM0"          , "VC_RXD0"      , "VC_SPI3_SCLK"    , }, // 7
    { "UART_RTS_1"      , "VC_RTS4"           , "VC_I2CSL_MOSI"    , "CTL_HDMI_5V"       , "VC_RTS0"      , "VC_SPI3_MOSI"    , }, // 8
    { "UART_CTS_1"      , "VC_CTS4"           , "VC_I2CSL_CE_N"    , "AON_PWM1"          , "VC_CTS0"      , "VC_SPI3_MISO"    , }, // 9
    { "TSIO_CLK_OUT"    , "CTL_HDMI_5V"       , "SC0_AUX1"         , "SPDIF_OUT"         , "VC_SPI5_CE1_N", "USB_PWRFLT"      , "AON_GPCLK"     , "SD_CARD_VOLT_F", }, // 10
    { "TSIO_DATA_IN"    , "UART_RTS_0"        , "SC0_AUX2"         , "AUD_FS_CLK0"       , "VC_SPI5_CE0_N", "USB_VBUS_PRESENT", "VC_RTS2"       , "SD_CARD_PWR0_F", }, // 11
    { "TSIO_DATA_OUT"   , "UART_CTS_0"        , "VC_RTS0"          , "TSIO_VCTRL"        , "VC_SPI5_MISO" , "USB_PWRON"       , "VC_CTS2"       , "SD_CARD_PRES_F", }, // 12
    { "BSC_M1_SDA"      , "UART_TXD_0"        , "VC_TXD0"          , "UUI_TXD"           , "VC_SPI5_MOSI" , "ARM_TMS"         , "VC_TXD2"       , "VC_SDA3"       , }, // 13
    { "BSC_M1_SCL"      , "UART_RXD_0"        , "VC_RXD0"          , "UUI_RXD"           , "VC_SPI5_SCLK" , "ARM_TCK"         , "VC_RXD2"       , "VC_SCL3"       , }, // 14
    { "IR_IN"           , "AON_FP_4SEC_RESETB", "VC_CTS0"          , "PM_LED_OUT"        , "CTL_HDMI_5V"  , "AON_PWM0"        , "AON_GPCLK"     , }, // 15
    { "AON_CPU_STANDBYB", "GPCLK0"            , "PM_LED_OUT"       , "CTL_HDMI_5V"       , "VC_PWM0_0"    , "USB_PWRON"       , "AUD_FS_CLK0"   , }, // 16

    // Pad out the bank to 32 entries
    { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, // 17-23
    { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, // 24-31

    { "HDMI_TX0_BSC_SCL", "HDMI_TX0_AUTO_I2C_SCL", "BSC_M0_SCL", "VC_SCL0", }, // sgpio 0
    { "HDMI_TX0_BSC_SDA", "HDMI_TX0_AUTO_I2C_SDA", "BSC_M0_SDA", "VC_SDA0", }, // sgpio 1
    { "HDMI_TX1_BSC_SCL", "HDMI_TX1_AUTO_I2C_SCL", "BSC_M1_SCL", "VC_SCL4", "CTL_HDMI_5V", }, // sgpio 2
    { "HDMI_TX1_BSC_SDA", "HDMI_TX1_AUTO_I2C_SDA", "BSC_M1_SDA", "VC_SDA4", }, // sgpio 3
    { "AVS_PMU_BSC_SCL", "BSC_M2_SCL", "VC_SCL5", "CTL_HDMI_5V", }, // sgpio 4
    { "AVS_PMU_BSC_SDA", "BSC_M2_SDA", "VC_SDA5", }, // sgpio 5
};

static const char *bcm2712D0AonGpioAltNames[][BCM2712_FSEL_COUNT - 1] =
{
    { "IR_IN"           , "VC_SPI0_CE1_N"     , "VC_TXD0"          , "VC_SDA3"           , "UART_TXD_0"   , "VC_SDA0"         , }, // 0
    { "VC_PWM0_0"       , "VC_SPI0_CE0_N"     , "VC_RXD0"          , "VC_SCL3"           , "UART_RXD_0"   , "AON_PWM0"        , "VC_SCL0"       , "VC_PWM1_0"     , }, // 1
    { "VC_PWM0_1"       , "VC_SPI0_MISO"      , "VC_CTS0"          , "CTL_HDMI_5V"       , "UART_CTS_0"   , "AON_PWM1"        , "IR_IN"         , "VC_PWM1_1"     , }, // 2
    { "IR_IN"           , "VC_SPI0_MOSI"      , "VC_RTS0"          , "UART_RTS_0"        , "SD_CARD_VOLT_G"  , "AON_GPCLK"     , }, // 3
    { "GPCLK0"          , "VC_SPI0_SCLK"      , "PM_LED_OUT"       , "AON_PWM0"          , "SD_CARD_PWR0_G"  , "VC_PWM0_0"     , }, // 4
    { "GPCLK1"          , "IR_IN"             , "AON_PWM1"         , "SD_CARD_PRES_G"    , "VC_PWM0_1"       , }, // 5
    { "UART_TXD_1"      , "VC_TXD2"           , "CTL_HDMI_5V"      , "GPCLK2"            , "VC_SPI3_CE0_N"   , }, // 6
    { "" }, // 7
    { "UART_RTS_1"      , "VC_RTS2"           , "CTL_HDMI_5V"      , "VC_SPI0_CE1_N"     , "VC_SPI3_SCLK"    , }, // 8
    { "UART_CTS_1"      , "VC_CTS2"           , "VC_CTS0"          , "AON_PWM1"          , "VC_SPI0_CE0_N"   , "VC_RTS2"      , "VC_SPI3_MOSI"  , }, // 9
    { "" }, // 10
    { "" }, // 11
    { "UART_RXD_1"      , "VC_RXD2"           , "VC_RTS0"          , "VC_SPI0_MISO"      , "USB_PWRON"       , "VC_CTS2"      , "VC_SPI3_MISO"  , }, // 12
    { "BSC_M1_SDA"      , "VC_TXD0"           , "UUI_TXD"          , "VC_SPI0_MOSI"      , "ARM_TMS"         , "VC_TXD2"      , "VC_SDA3"       , }, // 13
    { "BSC_M1_SCL"      , "AON_GPCLK"         , "VC_RXD0"          , "UUI_RXD"           , "VC_SPI0_SCLK"    , "ARM_TCK"      , "VC_RXD2"       , "VC_SCL3"       , }, // 14

    // Pad out the bank to 32 entries
    { "" }, // 15
    { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, // 16-23
    { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, { "" }, // 24-31

    { "HDMI_TX0_BSC_SCL", "HDMI_TX0_AUTO_I2C_SCL", "BSC_M0_SCL", "VC_SCL0", }, // sgpio 0
    { "HDMI_TX0_BSC_SDA", "HDMI_TX0_AUTO_I2C_SDA", "BSC_M0_SDA", "VC_SDA0", }, // sgpio 1
    { "HDMI_TX1_BSC_SCL", "HDMI_TX1_AUTO_I2C_SCL", "BSC_M1_SCL", "VC_SCL0", "CTL_HDMI_5V", }, // sgpio 2
    { "HDMI_TX1_BSC_SDA", "HDMI_TX1_AUTO_I2C_SDA", "BSC_M1_SDA", "VC_SDA0", }, // sgpio 3
    { "AVS_PMU_BSC_SCL", "BSC_M2_SCL", "VC_SCL3", "CTL_HDMI_5V", }, // sgpio 4
    { "AVS_PMU_BSC_SDA", "BSC_M2_SDA", "VC_SDA3", }, // sgpio 5
};

static const int bcm2712GpioD0ToC0[] =
{
    -1, 0, 1, 2, 3, -1, -1, -1, -1, -1,
    4, 5, 6, 7, 8, 9, -1, -1, 10, 11,
    12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27
};

static const int bcm2712GpioAonD0ToC0[] =
{
    0, 1, 2, 3, 4, 5, 6, -1, 7, 8,
    -1, -1, 9, 10, 11, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1,
    32, 33, 34, 35, 36, 37
};

// Returns a pointer to the 32-bit GPIO data/level register bank that GPIO 'gpio' lives in,
// with its bit position within that register written to *bitP.
// Returns NULL if 'gpio' is out of range for this instance, or if the instance's GPIO register
// window was never mapped.
static volatile uint32_t *
bcm2712GpioBase(BCM2712_INST_T *instP, unsigned gpio, unsigned int *bitP)
{
unsigned bank;

    bank = gpio / 32;
    gpio %= 32;

    if( (bank >= instP->numBanks) || (gpio >= instP->bankWidthsP[bank]) || !instP->gpioBaseP )
    {
        return(NULL);
    }

    *bitP = gpio;
    return(instP->gpioBaseP + bank * (0x20 / 4));
}

// Returns a pointer to the 32-bit pinmux register holding GPIO 'gpio's function-select
// field, with the field's bit offset written to *bitP.
// Handles the D0-stepping GPIO renumbering (translated back to C0 numbering via bcm2712GpioD0ToC0[]/
// bcm2712GpioAonD0ToC0[] before indexing) and the AON bank's irregular register layout.
// Returns NULL if 'gpio' is out of range, the pinmux register window was never mapped, or
// (D0 only) 'gpio' has no C0-numbering equivalent.
static volatile uint32_t *
bcm2712PinmuxBase(BCM2712_INST_T *instP, unsigned gpio, unsigned int *bitP)
{
unsigned bank, gpioOffset;

    if( (gpio >= instP->numGpios) || !instP->pinmuxBaseP )
    {
        return(NULL);
    }

    if( instP->flags & FLAGS_D0 )
    {
        if( instP->flags & FLAGS_AON )
        {
            gpio = bcm2712GpioAonD0ToC0[gpio];
        }
        else
        {
            gpio = bcm2712GpioD0ToC0[gpio];
        }

        if( (int)gpio < 0 )
        {
            return(NULL);
        }
    }

    bank = gpio / 32;
    gpioOffset = gpio % 32;

    if( (bank >= instP->numBanks) || (gpioOffset >= instP->bankWidthsP[bank]) )
    {
        return(NULL);
    }

    if( instP->flags & FLAGS_AON )
    {
        if( bank == 1 )
        {
            if( gpioOffset == 4 )
            {
                *bitP = 0;
                return(instP->pinmuxBaseP + 1);
            }
            else if( gpioOffset == 5 )
            {
                *bitP = 0;
                return(instP->pinmuxBaseP + 2);
            }
            else
            {
                *bitP = gpioOffset * 4;
                return(instP->pinmuxBaseP);
            }
        }

        *bitP = (gpioOffset % 8) * 4;
        return(instP->pinmuxBaseP + 3 + (gpioOffset / 8));
    }

    *bitP = (gpioOffset % 8) * 4;
    return(instP->pinmuxBaseP + (bank * 4) + (gpioOffset / 8));
}

// Returns a pointer to the 32-bit pad-control register holding GPIO 'gpio's pull setting
// with the field's bit offset written to *bitP.
// Same D0-numbering translation as bcm2712PinmuxBase().
// Returns NULL if 'gpio' is out of range, the pad register window was never mapped,
// (D0 only) 'gpio' has no C0-numbering equivalent, or the instance is an AON
// SGPIO bank.
static volatile uint32_t *
bcm2712PadBase(BCM2712_INST_T *instP, unsigned gpio, unsigned int *bitP)
{
unsigned bank, gpioOffset;

    if( (gpio >= instP->numGpios) || !instP->pinmuxBaseP )
    {
        return(NULL);
    }

    if( instP->flags & FLAGS_D0 )
    {
        if( instP->flags & FLAGS_AON )
        {
            gpio = bcm2712GpioAonD0ToC0[gpio];
        }
        else
        {
            gpio = bcm2712GpioD0ToC0[gpio];
        }

        if( (int)gpio < 0 )
        {
            return(NULL);
        }
    }

    bank = gpio / 32;
    gpioOffset = gpio % 32;

    if( (bank >= instP->numBanks) || (gpioOffset >= instP->bankWidthsP[bank]) )
    {
        return(NULL);
    }

    if( (instP->flags & FLAGS_AON) && (bank > 0) )
    {
        /* There is no SGPIO pad control (that I know of) */
        return(NULL);
    }

    gpio = gpioOffset + instP->padOffset;
    *bitP = (gpio % 15) * 2;
    return(instP->pinmuxBaseP + (gpio / 15));
}

// Returns the observed input level (0 or 1) of GPIO 'gpio', or -1 if 'gpio' is out of range.
static int
bcm2712GetLevel(void *priv, unsigned gpio)
{
BCM2712_INST_T *instP;
unsigned int bit;
volatile uint32_t *gpioBaseP;

    instP = priv;
    gpioBaseP = bcm2712GpioBase(instP, gpio, &bit);

    if( !gpioBaseP )
    {
        return(-1);
    }

    return( !!(gpioBaseP[BCM2712_GIO_DATA / 4] & (1 << bit)) );
}

// Drives GPIO 'gpio' high or low via the BCM2712's GIO_DATA register.
static void
bcm2712SetDrive(void *priv, unsigned gpio, GPIO_DRIVE_T drv)
{
BCM2712_INST_T *instP;
unsigned int bit;
volatile uint32_t *gpioBaseP;
uint32_t gpioVal;

    instP = priv;
    gpioBaseP = bcm2712GpioBase(instP, gpio, &bit);

    if( !gpioBaseP )
    {
        return;
    }

    gpioVal = gpioBaseP[BCM2712_GIO_DATA / 4];
    gpioVal = (gpioVal & ~(1U << bit)) | (drv << bit);
    gpioBaseP[BCM2712_GIO_DATA / 4] = gpioVal;
}

// Drives 'count' GPIOs high or low.
// This chip's GIO_DATA register is a read-modify-write, not a SET/CLR-alias pair like BCM2835/2711 or RP1,
// so there is no cheap masked-write path here worth building.
// Implemented as a plain loop over/ bcm2712SetDrive() purely so gpio_set_multi_drive is never
// a NULL function pointer for a chip that implements the rest of this interface.
static void
bcm2712SetMultiDrive(void *priv, const uint32_t *gpios, const GPIO_DRIVE_T *drvs, int count)
{
int i;

    for(i = 0; i < count; i++)
    {
        bcm2712SetDrive(priv, gpios[i], drvs[i]);
    }
}

// Returns the current drive level (DRIVE_LOW/DRIVE_HIGH) of GPIO 'gpio' as last written to
// GIO_DATA, or DRIVE_MAX if 'gpio' is out of range.
static GPIO_DRIVE_T
bcm2712GetDrive(void *priv, unsigned gpio)
{
BCM2712_INST_T *instP;
unsigned int bit;
volatile uint32_t *gpioBaseP;
uint32_t gpioVal;

    instP = priv;
    gpioBaseP = bcm2712GpioBase(instP, gpio, &bit);

    if( !gpioBaseP )
    {
        return(DRIVE_MAX);
    }

    gpioVal = gpioBaseP[BCM2712_GIO_DATA / 4];
    return( (gpioVal & (1U << bit)) ? DRIVE_HIGH : DRIVE_LOW );
}

// Sets the direction of GPIO 'gpio' via the BCM2712's GIO_IODIR register. No return value;
// does nothing if 'gpio' is out of range.
static void
bcm2712SetDir(void *priv, unsigned gpio, GPIO_DIR_T dir)
{
BCM2712_INST_T *instP;
unsigned int bit;
volatile uint32_t *gpioBaseP;
uint32_t gpioVal;

    instP = priv;
    gpioBaseP = bcm2712GpioBase(instP, gpio, &bit);

    if( !gpioBaseP )
    {
        return;
    }

    gpioVal = gpioBaseP[BCM2712_GIO_IODIR / 4];
    gpioVal &= ~(1U << bit);
    gpioVal |= ((dir == DIR_INPUT) << bit);
    gpioBaseP[BCM2712_GIO_IODIR / 4] = gpioVal;
}

// Returns the current direction (DIR_INPUT/DIR_OUTPUT) of GPIO 'gpio', or DIR_MAX if 'gpio'
// is out of range.
static GPIO_DIR_T
bcm2712GetDir(void *priv, unsigned gpio)
{
BCM2712_INST_T *instP;
unsigned int bit;
volatile uint32_t *gpioBaseP;
uint32_t gpioVal;

    instP = priv;
    gpioBaseP = bcm2712GpioBase(instP, gpio, &bit);

    if( !gpioBaseP )
    {
        return(DIR_MAX);
    }

    gpioVal = gpioBaseP[BCM2712_GIO_IODIR / 4];
    return( (gpioVal & (1U << bit)) ? DIR_INPUT : DIR_OUTPUT );
}

// Returns the current function-select value of GPIO 'gpio', decoded from the pinmux
// register's 4-bit field: 0 maps to GPIO_FSEL_GPIO, 0xf maps to GPIO_FSEL_NONE, and anything
// else in range maps to GPIO_FSEL_FUNC1 onward.
// Returns -1 if the pinmux register is unavailable or the raw field value is otherwise unrecognized.
static GPIO_FSEL_T
bcm2712GetFsel(void *priv, unsigned gpio)
{
BCM2712_INST_T *instP;
unsigned int pinmuxBit;
volatile uint32_t *pinmuxBaseP;
int fsel;

    instP = priv;
    pinmuxBaseP = bcm2712PinmuxBase(instP, gpio, &pinmuxBit);

    if( !pinmuxBaseP )
    {
        return(-1);
    }

    fsel = ((*pinmuxBaseP >> pinmuxBit) & 0xf);

    if( fsel == 0 )
    {
        return(GPIO_FSEL_GPIO);
    }
    else if( fsel < BCM2712_FSEL_COUNT )
    {
        return(GPIO_FSEL_FUNC1 + (fsel - 1));
    }
    else if( fsel == 0xf ) // Choose one value as a considered NONE
    {
        return(GPIO_FSEL_NONE);
    }

    /* Unknown FSEL */
    return(-1);
}

// Sets the function-select of GPIO 'gpio' to 'func'.
// A request for INPUT/OUTPUT/GPIO is treated as "last/current GPIO direction" (fsel raw value 0),
// additionally updating the/ GIO_IODIR direction bit for INPUT/OUTPUT.
// A request for FUNC0-FUNC8 sets the corresponding raw alternate-function code directly.
// Does nothing if the pinmux register/ is unavailable or 'func' is neither of the above.
static void
bcm2712SetFsel(void *priv, unsigned gpio, const GPIO_FSEL_T func)
{
BCM2712_INST_T *instP;
unsigned int pinmuxBit;
volatile uint32_t *pinmuxBaseP;
uint32_t pinmuxVal;
int fsel;

    instP = priv;
    pinmuxBaseP = bcm2712PinmuxBase(instP, gpio, &pinmuxBit);

    if( !pinmuxBaseP )
    {
        return;
    }

    if( (func == GPIO_FSEL_INPUT) || (func == GPIO_FSEL_OUTPUT) || (func == GPIO_FSEL_GPIO) )
    {
        // Set direction before switching
        // N.B. We explicitly interpret a request for FUNC_A0/GPIO as "last/current GPIO dir"
        fsel = 0;
        if( func == GPIO_FSEL_INPUT )
        {
            bcm2712SetDir(priv, gpio, DIR_INPUT);
        }
        else if( func == GPIO_FSEL_OUTPUT )
        {
            bcm2712SetDir(priv, gpio, DIR_OUTPUT);
        }
    }
    else if( (func >= GPIO_FSEL_FUNC0) && (func <= GPIO_FSEL_FUNC8) )
    {
        fsel = func - GPIO_FSEL_FUNC0;
    }
    else
    {
        return;
    }

    pinmuxVal = *pinmuxBaseP;
    pinmuxVal &= ~(0xf << pinmuxBit);
    pinmuxVal |= (fsel << pinmuxBit);
    *pinmuxBaseP = pinmuxVal;
}

// Returns the current pull resistor setting of GPIO 'gpio' from the pad-control register.
// Returns PULL_MAX if the pad register is unavailable (see bcm2712PadBase()) or the raw
// field value is otherwise unrecognized.
static GPIO_PULL_T
bcm2712GetPull(void *priv, unsigned gpio)
{
BCM2712_INST_T *instP;
unsigned int bit;
volatile uint32_t *padBaseP;
uint32_t padVal;

    instP = priv;
    padBaseP = bcm2712PadBase(instP, gpio, &bit);

    if( !padBaseP )
    {
        return(PULL_MAX);
    }

    padVal = (*padBaseP >> bit) & 0x3;
    switch( padVal )
    {
    case BCM2712_PAD_PULL_OFF:
        return(PULL_NONE);

    case BCM2712_PAD_PULL_DOWN:
        return(PULL_DOWN);

    case BCM2712_PAD_PULL_UP:
        return(PULL_UP);

    default:
        return(PULL_MAX); /* This is an error */
    }
}

// Sets the pull resistor setting of GPIO 'gpio' via the pad-control register.
// Does nothing if the pad register is unavailable.
// Asserts if/ 'pull' is not a valid GPIO_PULL_T value.
static void
bcm2712SetPull(void *priv, unsigned gpio, GPIO_PULL_T pull)
{
BCM2712_INST_T *instP;
unsigned int bit;
volatile uint32_t *padBaseP;
uint32_t padVal;
int val;

    bit = 0;
    instP = priv;
    padBaseP = bcm2712PadBase(instP, gpio, &bit);

    if( !padBaseP )
    {
        return;
    }

    switch( pull )
    {
    case PULL_NONE:
        val = BCM2712_PAD_PULL_OFF;
        break;

    case PULL_DOWN:
        val = BCM2712_PAD_PULL_DOWN;
        break;

    case PULL_UP:
        val = BCM2712_PAD_PULL_UP;
        break;

    default:
        assert(0);
        return;
    }

    padVal = *padBaseP;
    padVal &= ~(3 << bit);
    padVal |= (val << bit);

    *padBaseP = padVal;
}

// Allocates/locates the shared BCM2712_INST_T for a "gpio"-side device-tree node.
// Reads the per-bank GPIO widths from 'dtnode', infers AON/C0/D0 flags from the bank layout,falling
// back to inspecting gpio-line-names when a full 32+ wide non-AON bank could be either C0 or
// D0, then matches by AON-ness or allocates an instance shared with the corresponding
// pinctrl-side registration.
// Returns the instance cast to (void *) as the chip's 'priv' value, or NULL if the device-tree data
// is missing/invalid, a duplicate gpio node is found, or the instance table (BCM2712_MAX_INSTANCES) is full.
static void *
bcm2712CreateInstance(const GPIO_CHIP_T *chip, const char *dtnode)
{
BCM2712_INST_T *instP;
uint32_t *widthsP;
unsigned numBanks, instGpios;
unsigned flags;
unsigned bank;
unsigned i;

    instP = NULL;
    flags = FLAGS_GPIO | chip->data;

    widthsP = dt_read_cells(dtnode, "brcm,gpio-bank-widths", &numBanks);
    if( !widthsP )
    {
        return(NULL);
    }

    instGpios = 0;
    for( bank = 0; bank < numBanks; bank++ )
    {
        instGpios = ROUND_UP(instGpios, 32) + widthsP[bank];
    }

    flags |= sharedFlags;
    if( widthsP[0] < 32 )
    {
        flags |= FLAGS_AON;
        if( widthsP[0] == 15 )
        {
            flags |= FLAGS_D0;
        }
        else
        {
            flags |= FLAGS_C0;
        }
    }
    else if( !(flags & (FLAGS_C0 | FLAGS_D0)) )
    {
        size_t namesLen;
        char *namesP;

        namesP = dt_read_prop(dtnode, "gpio-line-names", &namesLen);
        if( !namesP[0] )
        {
            flags |= FLAGS_D0;
        }

        dt_free(namesP);
    }

    sharedFlags |= (flags & (FLAGS_C0 | FLAGS_D0));

    /* look for a corresponding pinctrl instance */
    for( i = 0; i < numInstances; i++ )
    {
        BCM2712_INST_T *pinctrlInstP;

        pinctrlInstP = &bcm2712Instances[i];
        pinctrlInstP->flags |= sharedFlags;

        if( !((pinctrlInstP->flags ^ flags) & FLAGS_AON) )
        {
            if( pinctrlInstP->flags & FLAGS_GPIO )
            {
                assert(!"duplicate gpio nodes?");
                return(NULL);
            }

            instP = pinctrlInstP;
            break;
        }
    }

    if( !instP )
    {
        if( numInstances == BCM2712_MAX_INSTANCES )
        {
            return(NULL);
        }

        instP = &bcm2712Instances[numInstances++];
    }

    instP->numGpios = instGpios;
    instP->numBanks = numBanks;
    instP->bankWidthsP = widthsP;
    instP->flags |= flags;

    return( (void *)instP );
}

// Returns the number of GPIOs this "gpio"-side instance provides.
static int
bcm2712GpioCount(void *priv)
{
BCM2712_INST_T *instP;

    instP = priv;

    return(instP->numGpios);
}

// Completes "gpio"-side instance setup once the physical GPIO register window has been
// mmap()'d to 'base'.
// Returns the instance, never NULL.
static void *
bcm2712ProbeInstance(void *priv, volatile uint32_t *base)
{
BCM2712_INST_T *instP;

    instP = priv;
    instP->gpioBaseP = base;

    return(instP);
}

// Allocates/locates the shared BCM2712_INST_T for a "pinctrl"-side device-tree node,
// cross-checking the node's declared register-window size against the AON/C0/D0 flags baked
// into this chip's DECLARE_GPIO_CHIP() registration.
// Returns the instance cast to (void *) as the chip's 'priv' value, or
// NULL if the device-tree data is missing/invalid, a duplicate pinctrl node is found, or the
// instance table (BCM2712_MAX_INSTANCES) is full.
static void *
bcm2712PinctrlCreateInstance(const GPIO_CHIP_T *chip, const char *dtnode)
{
BCM2712_INST_T *instP;
unsigned flags;
unsigned regCells, regSize;
uint32_t *regP;
unsigned i;

    instP = NULL;
    flags = FLAGS_PINCTRL | chip->data;

    if( dtnode )
    {
        regP = dt_read_cells(dtnode, "reg", &regCells);
        if( !regP || (regCells < 2) )
        {
            return(NULL);
        }

        regSize = (regCells > 1) ? regP[regCells - 1] : 0;
        dt_free(regP);

        switch( regSize )
        {
        case 0x1c:
            assert((flags & FLAGS_AON) && (flags & FLAGS_D0));
            break;

        case 0x20:
            assert( ((flags & FLAGS_AON) && !(flags & FLAGS_D0)) ||
                (!(flags & FLAGS_AON) && (flags & FLAGS_D0)) );
            break;

        case 0x30:
            assert(!(flags & FLAGS_AON) && !(flags & FLAGS_D0));
            break;

        default:
            assert(0);
        }
    }

    sharedFlags |= (flags & (FLAGS_C0 | FLAGS_D0));

    /* look for a corresponding gpio instance */
    for( i = 0; i < numInstances; i++ )
    {
        BCM2712_INST_T *gpioInstP;

        gpioInstP = &bcm2712Instances[i];
        gpioInstP->flags |= sharedFlags;

        if( !((gpioInstP->flags ^ flags) & FLAGS_AON) )
        {
            if( gpioInstP->flags & FLAGS_PINCTRL )
            {
                assert(!"duplicate pinctrl nodes?");
                return(NULL);
            }

            instP = gpioInstP;
            break;
        }
    }

    if( !instP )
    {
        if( numInstances == BCM2712_MAX_INSTANCES )
        {
            return(NULL);
        }

        instP = &bcm2712Instances[numInstances++];
    }

    instP->flags |= flags;

    return( (void *)instP );
}

// Returns the number of GPIOs this "pinctrl"-side instance provides.
// A pinctrl node paired with a "gpio" node contributes 0, the gpio side already counted them.
// A standalone pinctrl node infers the count from its AON/C0/D0 flags.
static int
bcm2712PinctrlCount(void *priv)
{
BCM2712_INST_T *instP;

    instP = priv;

    if( instP->flags & FLAGS_GPIO )
    {
        return(0);  /* Don't occupy any GPIO space */
    }

    if( !instP->numGpios )
    {
        switch( instP->flags & (FLAGS_AON | FLAGS_C0 | FLAGS_D0) )
        {
        case 0:
        case FLAGS_C0:
            instP->numGpios = 54;
            break;

        case FLAGS_D0:
            instP->numGpios = 36;
            break;

        case FLAGS_AON:
        case FLAGS_AON | FLAGS_D0:
        case FLAGS_AON | FLAGS_C0:
            instP->numGpios = 38;
            break;

        default:
            break;
        }
    }

    return(instP->numGpios);
}

// Completes "pinctrl"-side instance setup once the physical pinmux register window has been
// mmap()'d to 'base', and derives the pad-control offset into that same window for this
// AON/C0/D0 combination.
// Returns the instance, never NULL.
static void *
bcm2712PinctrlProbeInstance(void *priv, volatile uint32_t *base)
{
BCM2712_INST_T *instP;
unsigned padOffset;

    instP = priv;
    instP->pinmuxBaseP = base;

    switch( instP->flags & (FLAGS_D0 | FLAGS_C0 | FLAGS_AON) )
    {
    case FLAGS_C0:
    default:
        padOffset = 112;
        break;

    case FLAGS_D0:
        padOffset = 65;
        break;

    case FLAGS_AON:
    case FLAGS_C0 | FLAGS_AON:
        padOffset = 100;
        break;

    case FLAGS_D0 | FLAGS_AON:
        padOffset = 84;
        break;
    }

    instP->padOffset = padOffset;

    return(instP);
}

// Returns a human-readable name for what function-select value 'fsel' means on GPIO 'gpio'.
// Ppecifically: "gpio" for GPIO_FSEL_GPIO/FUNC0, "input"/"output"/"none" for the
// corresponding portable requests, or a chip/stepping-specific alternate-function name from
// whichever of the four bcm2712*AltNames tables matches this instance's AON/C0/D0
// combination for FUNC1-FUNC8.
// Returns NULL if 'fsel' is not recognized or 'gpio' is out of range.
static const char *
bcm2712GetFselName(void *priv, unsigned gpio, GPIO_FSEL_T fsel)
{
BCM2712_INST_T *instP;
const char *nameP;

    instP = priv;
    nameP = NULL;

    switch( fsel )
    {
    case GPIO_FSEL_GPIO:
    case GPIO_FSEL_FUNC0:
        nameP = "gpio";
        break;

    case GPIO_FSEL_INPUT:
        nameP = "input";
        break;

    case GPIO_FSEL_OUTPUT:
        nameP = "output";
        break;

    case GPIO_FSEL_NONE:
        nameP = "none";
        break;

    case GPIO_FSEL_FUNC1:
    case GPIO_FSEL_FUNC2:
    case GPIO_FSEL_FUNC3:
    case GPIO_FSEL_FUNC4:
    case GPIO_FSEL_FUNC5:
    case GPIO_FSEL_FUNC6:
    case GPIO_FSEL_FUNC7:
    case GPIO_FSEL_FUNC8:
        if( gpio < instP->numGpios )
        {
            switch( instP->flags & (FLAGS_AON | FLAGS_C0 | FLAGS_D0) )
            {
            case FLAGS_C0 | FLAGS_AON:
            case FLAGS_AON:
                nameP = bcm2712C0AonGpioAltNames[gpio][fsel - 1];
                break;

            case FLAGS_C0:
            case 0:
                nameP = bcm2712C0GpioAltNames[gpio][fsel - 1];
                break;

            case FLAGS_D0 | FLAGS_AON:
                nameP = bcm2712D0AonGpioAltNames[gpio][fsel - 1];
                break;

            case FLAGS_D0:
                nameP = bcm2712D0GpioAltNames[gpio][fsel - 1];
                break;
            }

            if( !nameP )
            {
                nameP = "-";
            }
        }
        break;

    default:
        break;
    }

    return(nameP);
}

// Returns a human-readable name for GPIO 'gpio', "GPIOn"/"AON_GPIOn"/"AON_SGPIOn" as
// appropriate), formatted into a static buffer.
// Returns NULL if 'gpio' has no alternate-function name at all, or is out of range for a "gpio"-side
// instance's declared bank widths.
static const char *
bcm2712GetName(void *priv, unsigned gpio)
{
BCM2712_INST_T *instP;
const char *fselNameP;
static char nameBuf[16];
unsigned gpioOffset;
unsigned bank;

    instP = priv;

    fselNameP = bcm2712GetFselName(priv, gpio, GPIO_FSEL_FUNC1);
    if( !fselNameP || !fselNameP[0] )
    {
        return(NULL);
    }

    bank = gpio / 32;
    gpioOffset = gpio % 32;

    if( (instP->flags & FLAGS_GPIO) &&
        ((bank >= instP->numBanks) || (gpioOffset >= instP->bankWidthsP[bank])) )
    {
        return(NULL);
    }

    if( instP->flags & FLAGS_AON )
    {
        if( bank == 1 )
        {
            sprintf(nameBuf, "AON_SGPIO%d", gpioOffset);
        }
        else
        {
            sprintf(nameBuf, "AON_GPIO%d", gpioOffset);
        }
    }
    else
    {
        sprintf(nameBuf, "GPIO%d", gpio);
    }

    return(nameBuf);
}

static const GPIO_CHIP_INTERFACE_T bcm2712GpioInterface =
{
    .gpio_create_instance = bcm2712CreateInstance,
    .gpio_count = bcm2712GpioCount,
    .gpio_probe_instance = bcm2712ProbeInstance,
    .gpio_get_fsel = bcm2712GetFsel,
    .gpio_set_fsel = bcm2712SetFsel,
    .gpio_set_drive = bcm2712SetDrive,
    .gpio_set_multi_drive = bcm2712SetMultiDrive,
    .gpio_set_dir = bcm2712SetDir,
    .gpio_get_dir = bcm2712GetDir,
    .gpio_get_level = bcm2712GetLevel,
    .gpio_get_drive = bcm2712GetDrive,
    .gpio_get_pull = bcm2712GetPull,
    .gpio_set_pull = bcm2712SetPull,
    .gpio_get_name = bcm2712GetName,
    .gpio_get_fsel_name = bcm2712GetFselName,
};

DECLARE_GPIO_CHIP(brcmstb, "brcm,brcmstb-gpio", &bcm2712GpioInterface, 0x40, 0);

static const GPIO_CHIP_INTERFACE_T bcm2712PinctrlInterface =
{
    .gpio_create_instance = bcm2712PinctrlCreateInstance,
    .gpio_count = bcm2712PinctrlCount,
    .gpio_probe_instance = bcm2712PinctrlProbeInstance,
    .gpio_get_fsel = bcm2712GetFsel,
    .gpio_set_fsel = bcm2712SetFsel,
    .gpio_set_drive = bcm2712SetDrive,
    .gpio_set_multi_drive = bcm2712SetMultiDrive,
    .gpio_set_dir = bcm2712SetDir,
    .gpio_get_dir = bcm2712GetDir,
    .gpio_get_level = bcm2712GetLevel,
    .gpio_get_drive = bcm2712GetDrive,
    .gpio_get_pull = bcm2712GetPull,
    .gpio_set_pull = bcm2712SetPull,
    .gpio_get_name = bcm2712GetName,
    .gpio_get_fsel_name = bcm2712GetFselName,
};

DECLARE_GPIO_CHIP(bcm2712, "brcm,bcm2712-pinctrl", &bcm2712PinctrlInterface, 0x30, 0);
DECLARE_GPIO_CHIP(bcm2712_aon, "brcm,bcm2712-aon-pinctrl", &bcm2712PinctrlInterface, 0x20, FLAGS_AON);

DECLARE_GPIO_CHIP(bcm2712c0, "brcm,bcm2712c0-pinctrl", &bcm2712PinctrlInterface, 0x30, FLAGS_C0);
DECLARE_GPIO_CHIP(bcm2712c0_aon, "brcm,bcm2712c0-aon-pinctrl", &bcm2712PinctrlInterface, 0x20, FLAGS_C0 | FLAGS_AON);

DECLARE_GPIO_CHIP(bcm2712d0, "brcm,bcm2712d0-pinctrl", &bcm2712PinctrlInterface, 0x20, FLAGS_D0);
DECLARE_GPIO_CHIP(bcm2712d0_aon, "brcm,bcm2712d0-aon-pinctrl", &bcm2712PinctrlInterface, 0x1c, FLAGS_D0 | FLAGS_AON);
