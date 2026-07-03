// gpiochip_rp1.c -- GPIO_CHIP_T implementation for the RP1 southbridge chip (Pi 5's I/O
// companion, exposing IO_BANKn/SYS_RIO_BANKn/PADS_BANKn register blocks per GPIO bank). All
// functions/types here are file-local (static) and have been renamed to the project's
// camelHump convention; the DECLARE_GPIO_CHIP() invocation name ("rp1") is left as-is since
// it forms the externally-visible rp1_chip linker-section symbol name.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpiochip.h"
#include "util.h"

#define RP1_NUM_GPIOS 54

#define RP1_IO_BANK0_OFFSET      0x00000000
#define RP1_IO_BANK1_OFFSET      0x00004000
#define RP1_IO_BANK2_OFFSET      0x00008000
#define RP1_SYS_RIO_BANK0_OFFSET 0x00010000
#define RP1_SYS_RIO_BANK1_OFFSET 0x00014000
#define RP1_SYS_RIO_BANK2_OFFSET 0x00018000
#define RP1_PADS_BANK0_OFFSET    0x00020000
#define RP1_PADS_BANK1_OFFSET    0x00024000
#define RP1_PADS_BANK2_OFFSET    0x00028000

#define RP1_RW_OFFSET  0x0000
#define RP1_XOR_OFFSET 0x1000
#define RP1_SET_OFFSET 0x2000
#define RP1_CLR_OFFSET 0x3000

#define RP1_GPIO_CTRL_FSEL_LSB     0
#define RP1_GPIO_CTRL_FSEL_MASK    (0x1f << RP1_GPIO_CTRL_FSEL_LSB)
#define RP1_GPIO_CTRL_OUTOVER_LSB  12
#define RP1_GPIO_CTRL_OUTOVER_MASK (0x03 << RP1_GPIO_CTRL_OUTOVER_LSB)
#define RP1_GPIO_CTRL_OEOVER_LSB   14
#define RP1_GPIO_CTRL_OEOVER_MASK  (0x03 << RP1_GPIO_CTRL_OEOVER_LSB)

#define RP1_PADS_OD_SET       (1 << 7)
#define RP1_PADS_IE_SET       (1 << 6)
#define RP1_PADS_PUE_SET      (1 << 3)
#define RP1_PADS_PDE_SET      (1 << 2)

#define RP1_GPIO_IO_REG_STATUS_OFFSET(offset) (((offset * 2) + 0) * sizeof(uint32_t))
#define RP1_GPIO_IO_REG_CTRL_OFFSET(offset)   (((offset * 2) + 1) * sizeof(uint32_t))
#define RP1_GPIO_PADS_REG_OFFSET(offset)      (sizeof(uint32_t) + (offset * sizeof(uint32_t)))

#define RP1_GPIO_SYS_RIO_REG_OUT_OFFSET        0x0
#define RP1_GPIO_SYS_RIO_REG_OE_OFFSET         0x4
#define RP1_GPIO_SYS_RIO_REG_SYNC_IN_OFFSET    0x8

#define rp1GpioWrite32(base, peri_offset, reg_offset, value) \
    base[(peri_offset + reg_offset) / 4] = value

#define rp1GpioRead32(base, peri_offset, reg_offset) \
    base[(peri_offset + reg_offset) / 4]

typedef struct
{
    uint32_t io[3];
    uint32_t pads[3];
    uint32_t sys_rio[3];
} GPIO_STATE_T;

typedef enum
{
    RP1_FSEL_ALT0       = 0x0,
    RP1_FSEL_ALT1       = 0x1,
    RP1_FSEL_ALT2       = 0x2,
    RP1_FSEL_ALT3       = 0x3,
    RP1_FSEL_ALT4       = 0x4,
    RP1_FSEL_ALT5       = 0x5,
    RP1_FSEL_ALT6       = 0x6,
    RP1_FSEL_ALT7       = 0x7,
    RP1_FSEL_ALT8       = 0x8,
    RP1_FSEL_COUNT,
    RP1_FSEL_SYS_RIO    = RP1_FSEL_ALT5,
    RP1_FSEL_NULL       = 0x1f
} RP1_FSEL_T;

static const GPIO_STATE_T gpioState =
{
    .io = { RP1_IO_BANK0_OFFSET, RP1_IO_BANK1_OFFSET, RP1_IO_BANK2_OFFSET },
    .pads = { RP1_PADS_BANK0_OFFSET, RP1_PADS_BANK1_OFFSET, RP1_PADS_BANK2_OFFSET },
    .sys_rio = { RP1_SYS_RIO_BANK0_OFFSET, RP1_SYS_RIO_BANK1_OFFSET, RP1_SYS_RIO_BANK2_OFFSET },
};

static const int rp1BankBase[] = { 0, 28, 34 };

static const char *rp1GpioFselNames[RP1_NUM_GPIOS][RP1_FSEL_COUNT] =
{
    { "SPI0_SIO3" , "DPI_PCLK"     , "TXD1"         , "SDA0"         , 0              , "SYS_RIO00" , "PROC_RIO00" , "PIO0"       , "SPI2_CE0" , },
    { "SPI0_SIO2" , "DPI_DE"       , "RXD1"         , "SCL0"         , 0              , "SYS_RIO01" , "PROC_RIO01" , "PIO1"       , "SPI2_SIO1", },
    { "SPI0_CE3"  , "DPI_VSYNC"    , "CTS1"         , "SDA1"         , "IR_RX0"       , "SYS_RIO02" , "PROC_RIO02" , "PIO2"       , "SPI2_SIO0", },
    { "SPI0_CE2"  , "DPI_HSYNC"    , "RTS1"         , "SCL1"         , "IR_TX0"       , "SYS_RIO03" , "PROC_RIO03" , "PIO3"       , "SPI2_SCLK", },
    { "GPCLK0"    , "DPI_D0"       , "TXD2"         , "SDA2"         , "RI0"          , "SYS_RIO04" , "PROC_RIO04" , "PIO4"       , "SPI3_CE0" , },
    { "GPCLK1"    , "DPI_D1"       , "RXD2"         , "SCL2"         , "DTR0"         , "SYS_RIO05" , "PROC_RIO05" , "PIO5"       , "SPI3_SIO1", },
    { "GPCLK2"    , "DPI_D2"       , "CTS2"         , "SDA3"         , "DCD0"         , "SYS_RIO06" , "PROC_RIO06" , "PIO6"       , "SPI3_SIO0", },
    { "SPI0_CE1"  , "DPI_D3"       , "RTS2"         , "SCL3"         , "DSR0"         , "SYS_RIO07" , "PROC_RIO07" , "PIO7"       , "SPI3_SCLK", },
    { "SPI0_CE0"  , "DPI_D4"       , "TXD3"         , "SDA0"         , 0              , "SYS_RIO08" , "PROC_RIO08" , "PIO8"       , "SPI4_CE0" , },
    { "SPI0_MISO" , "DPI_D5"       , "RXD3"         , "SCL0"         , 0              , "SYS_RIO09" , "PROC_RIO09" , "PIO9"       , "SPI4_SIO0", },
    { "SPI0_MOSI" , "DPI_D6"       , "CTS3"         , "SDA1"         , 0              , "SYS_RIO010", "PROC_RIO010", "PIO10"      , "SPI4_SIO1", },
    { "SPI0_SCLK" , "DPI_D7"       , "RTS3"         , "SCL1"         , 0              , "SYS_RIO011", "PROC_RIO011", "PIO11"      , "SPI4_SCLK", },
    { "PWM0_CHAN0", "DPI_D8"       , "TXD4"         , "SDA2"         , "AAUD_LEFT"    , "SYS_RIO012", "PROC_RIO012", "PIO12"      , "SPI5_CE0" , },
    { "PWM0_CHAN1", "DPI_D9"       , "RXD4"         , "SCL2"         , "AAUD_RIGHT"   , "SYS_RIO013", "PROC_RIO013", "PIO13"      , "SPI5_SIO1", },
    { "PWM0_CHAN2", "DPI_D10"      , "CTS4"         , "SDA3"         , "TXD0"         , "SYS_RIO014", "PROC_RIO014", "PIO14"      , "SPI5_SIO0", },
    { "PWM0_CHAN3", "DPI_D11"      , "RTS4"         , "SCL3"         , "RXD0"         , "SYS_RIO015", "PROC_RIO015", "PIO15"      , "SPI5_SCLK", },
    { "SPI1_CE2"  , "DPI_D12"      , "DSI0_TE_EXT"  , 0              , "CTS0"         , "SYS_RIO016", "PROC_RIO016", "PIO16"      , },
    { "SPI1_CE1"  , "DPI_D13"      , "DSI1_TE_EXT"  , 0              , "RTS0"         , "SYS_RIO017", "PROC_RIO017", "PIO17"      , },
    { "SPI1_CE0"  , "DPI_D14"      , "I2S0_SCLK"    , "PWM0_CHAN2"   , "I2S1_SCLK"    , "SYS_RIO018", "PROC_RIO018", "PIO18"      , "GPCLK1",   },
    { "SPI1_MISO" , "DPI_D15"      , "I2S0_WS"      , "PWM0_CHAN3"   , "I2S1_WS"      , "SYS_RIO019", "PROC_RIO019", "PIO19"      , },
    { "SPI1_MOSI" , "DPI_D16"      , "I2S0_SDI0"    , "GPCLK0"       , "I2S1_SDI0"    , "SYS_RIO020", "PROC_RIO020", "PIO20"      , },
    { "SPI1_SCLK" , "DPI_D17"      , "I2S0_SDO0"    , "GPCLK1"       , "I2S1_SDO0"    , "SYS_RIO021", "PROC_RIO021", "PIO21"      , },
    { "SD0CLK"    , "DPI_D18"      , "I2S0_SDI1"    , "SDA3"         , "I2S1_SDI1"    , "SYS_RIO022", "PROC_RIO022", "PIO22"      , },
    { "SD0_CMD"   , "DPI_D19"      , "I2S0_SDO1"    , "SCL3"         , "I2S1_SDO1"    , "SYS_RIO023", "PROC_RIO023", "PIO23"      , },
    { "SD0_DAT0"  , "DPI_D20"      , "I2S0_SDI2"    , 0              , "I2S1_SDI2"    , "SYS_RIO024", "PROC_RIO024", "PIO24"      , "SPI2_CE1" , },
    { "SD0_DAT1"  , "DPI_D21"      , "I2S0_SDO2"    , "MIC_CLK"      , "I2S1_SDO2"    , "SYS_RIO025", "PROC_RIO025", "PIO25"      , "SPI3_CE1" , },
    { "SD0_DAT2"  , "DPI_D22"      , "I2S0_SDI3"    , "MIC_DAT0"     , "I2S1_SDI3"    , "SYS_RIO026", "PROC_RIO026", "PIO26"      , "SPI5_CE1" , },
    { "SD0_DAT3"  , "DPI_D23"      , "I2S0_SDO3"    , "MIC_DAT1"     , "I2S1_SDO3"    , "SYS_RIO027", "PROC_RIO027", "PIO27"      , "SPI1_CE1" , },
    { "SD1CLK"    , "SDA4"         , "I2S2_SCLK"    , "SPI6_MISO"    , "VBUS_EN0"     , "SYS_RIO10" , "PROC_RIO10" , },
    { "SD1_CMD"   , "SCL4"         , "I2S2_WS"      , "SPI6_MOSI"    , "VBUS_OC0"     , "SYS_RIO11" , "PROC_RIO11" , },
    { "SD1_DAT0"  , "SDA5"         , "I2S2_SDI0"    , "SPI6_SCLK"    , "TXD5"         , "SYS_RIO12" , "PROC_RIO12" , },
    { "SD1_DAT1"  , "SCL5"         , "I2S2_SDO0"    , "SPI6_CE0"     , "RXD5"         , "SYS_RIO13" , "PROC_RIO13" , },
    { "SD1_DAT2"  , "GPCLK3"       , "I2S2_SDI1"    , "SPI6_CE1"     , "CTS5"         , "SYS_RIO14" , "PROC_RIO14" , },
    { "SD1_DAT3"  , "GPCLK4"       , "I2S2_SDO1"    , "SPI6_CE2"     , "RTS5"         , "SYS_RIO15" , "PROC_RIO15" , },
    { "PWM1_CHAN2", "GPCLK3"       , "VBUS_EN0"     , "SDA4"         , "MIC_CLK"      , "SYS_RIO20" , "PROC_RIO20" , },
    { "SPI8_CE1"  , "PWM1_CHAN0"   , "VBUS_OC0"     , "SCL4"         , "MIC_DAT0"     , "SYS_RIO21" , "PROC_RIO21" , },
    { "SPI8_CE0"  , "TXD5"         , "PCIE_CLKREQ_N", "SDA5"         , "MIC_DAT1"     , "SYS_RIO22" , "PROC_RIO22" , },
    { "SPI8_MISO" , "RXD5"         , "MIC_CLK"      , "SCL5"         , "PCIE_CLKREQ_N", "SYS_RIO23" , "PROC_RIO23" , },
    { "SPI8_MOSI" , "RTS5"         , "MIC_DAT0"     , "SDA6"         , "AAUD_LEFT"    , "SYS_RIO24" , "PROC_RIO24" , "DSI0_TE_EXT", },
    { "SPI8_SCLK" , "CTS5"         , "MIC_DAT1"     , "SCL6"         , "AAUD_RIGHT"   , "SYS_RIO25" , "PROC_RIO25" , "DSI1_TE_EXT", },
    { "PWM1_CHAN1", "TXD5"         , "SDA4"         , "SPI6_MISO"    , "AAUD_LEFT"    , "SYS_RIO26" , "PROC_RIO26" , },
    { "PWM1_CHAN2", "RXD5"         , "SCL4"         , "SPI6_MOSI"    , "AAUD_RIGHT"   , "SYS_RIO27" , "PROC_RIO27" , },
    { "GPCLK5"    , "RTS5"         , "VBUS_EN1"     , "SPI6_SCLK"    , "I2S2_SCLK"    , "SYS_RIO28" , "PROC_RIO28" , },
    { "GPCLK4"    , "CTS5"         , "VBUS_OC1"     , "SPI6_CE0"     , "I2S2_WS"      , "SYS_RIO29" , "PROC_RIO29" , },
    { "GPCLK5"    , "SDA5"         , "PWM1_CHAN0"   , "SPI6_CE1"     , "I2S2_SDI0"    , "SYS_RIO210", "PROC_RIO210", },
    { "PWM1_CHAN3", "SCL5"         , "SPI7_CE0"     , "SPI6_CE2"     , "I2S2_SDO0"    , "SYS_RIO211", "PROC_RIO211", },
    { "GPCLK3"    , "SDA4"         , "SPI7_MOSI"    , "MIC_CLK"      , "I2S2_SDI1"    , "SYS_RIO212", "PROC_RIO212", "DSI0_TE_EXT", },
    { "GPCLK5"    , "SCL4"         , "SPI7_MISO"    , "MIC_DAT0"     , "I2S2_SDO1"    , "SYS_RIO213", "PROC_RIO213", "DSI1_TE_EXT", },
    { "PWM1_CHAN0", "PCIE_CLKREQ_N", "SPI7_SCLK"    , "MIC_DAT1"     , "TXD5"         , "SYS_RIO214", "PROC_RIO214", },
    { "SPI8_SCLK" , "SPI7_SCLK"    , "SDA5"         , "AAUD_LEFT"    , "RXD5"         , "SYS_RIO215", "PROC_RIO215", },
    { "SPI8_MISO" , "SPI7_MOSI"    , "SCL5"         , "AAUD_RIGHT"   , "VBUS_EN2"     , "SYS_RIO216", "PROC_RIO216", },
    { "SPI8_MOSI" , "SPI7_MISO"    , "SDA6"         , "AAUD_LEFT"    , "VBUS_OC2"     , "SYS_RIO217", "PROC_RIO217", },
    { "SPI8_CE0"  , 0              , "SCL6"         , "AAUD_RIGHT"   , "VBUS_EN3"     , "SYS_RIO218", "PROC_RIO218", },
    { "SPI8_CE1"  , "SPI7_CE0"     , 0              , "PCIE_CLKREQ_N", "VBUS_OC3"     , "SYS_RIO219", "PROC_RIO219", },
};

// Resolves absolute GPIO number 'num' into its bank index and per-bank offset (*bankP/
// *offsetP), using rp1BankBase[] to find which of the 3 banks 'num' falls in. No return
// value; asserts (and leaves *bankP/*offsetP at 0) if 'num' is out of range.
static void
rp1GpioGetBank(int num, int *bankP, int *offsetP)
{
    *bankP = 0;
    *offsetP = 0;

    if( num >= RP1_NUM_GPIOS )
    {
        assert(0);
        return;
    }

    if( num < rp1BankBase[1] )
    {
        *bankP = 0;
    }
    else if( num < rp1BankBase[2] )
    {
        *bankP = 1;
    }
    else
    {
        *bankP = 2;
    }

    *offsetP = num - rp1BankBase[*bankP];
}

// Reads the IO_BANKn CTRL register for GPIO offset 'offset' in bank 'bank' (fsel, override
// bits, etc.).
static uint32_t
rp1GpioCtrlRead(volatile uint32_t *baseP, int bank, int offset)
{
    return( rp1GpioRead32(baseP, gpioState.io[bank], RP1_GPIO_IO_REG_CTRL_OFFSET(offset)) );
}

// Writes 'value' to the IO_BANKn CTRL register for GPIO offset 'offset' in bank 'bank'. No
// return value.
static void
rp1GpioCtrlWrite(volatile uint32_t *baseP, int bank, int offset, uint32_t value)
{
    rp1GpioWrite32(baseP, gpioState.io[bank], RP1_GPIO_IO_REG_CTRL_OFFSET(offset), value);
}

// Reads the PADS_BANKn register for GPIO offset 'offset' in bank 'bank' (pull/drive-strength/
// input-enable bits).
static uint32_t
rp1GpioPadsRead(volatile uint32_t *baseP, int bank, int offset)
{
    return( rp1GpioRead32(baseP, gpioState.pads[bank], RP1_GPIO_PADS_REG_OFFSET(offset)) );
}

// Writes 'value' to the PADS_BANKn register for GPIO offset 'offset' in bank 'bank'. No
// return value.
static void
rp1GpioPadsWrite(volatile uint32_t *baseP, int bank, int offset, uint32_t value)
{
    rp1GpioWrite32(baseP, gpioState.pads[bank], RP1_GPIO_PADS_REG_OFFSET(offset), value);
}

// Reads the SYS_RIO_BANKn OUT register (the last value driven to every GPIO in 'bank' via
// the RIO output-set/output-clear mechanism).
static uint32_t
rp1GpioSysRioOutRead(volatile uint32_t *baseP, int bank, int offset)
{
    UNUSED(offset);
    return( rp1GpioRead32(baseP, gpioState.sys_rio[bank], RP1_GPIO_SYS_RIO_REG_OUT_OFFSET) );
}

// Reads the SYS_RIO_BANKn SYNC_IN register (the synchronized observed input level for every
// GPIO in 'bank').
static uint32_t
rp1GpioSysRioSyncInRead(volatile uint32_t *baseP, int bank, int offset)
{
    UNUSED(offset);
    return( rp1GpioRead32(baseP, gpioState.sys_rio[bank], RP1_GPIO_SYS_RIO_REG_SYNC_IN_OFFSET) );
}

// wje 02-Jul-26 - replaced rp1_gpio_sys_rio_out_write() with SET/CLR-register equivalents.
//  RP1's register blocks expose RW/XOR/SET/CLR variants at fixed offsets
//  precisely so a single GPIO can be changed with one atomic write and no read.

// Atomically drives GPIO offset 'offset' in bank 'bank' high via the SYS_RIO OUT SET-alias
// register. No return value.
static void
rp1GpioSysRioOutSet(volatile uint32_t *baseP, int bank, int offset)
{
    rp1GpioWrite32(baseP, gpioState.sys_rio[bank],
        RP1_GPIO_SYS_RIO_REG_OUT_OFFSET + RP1_SET_OFFSET, 1U << offset);
}

// Atomically drives GPIO offset 'offset' in bank 'bank' low via the SYS_RIO OUT CLR-alias
// register. No return value.
static void
rp1GpioSysRioOutClr(volatile uint32_t *baseP, int bank, int offset)
{
    rp1GpioWrite32(baseP, gpioState.sys_rio[bank],
        RP1_GPIO_SYS_RIO_REG_OUT_OFFSET + RP1_CLR_OFFSET, 1U << offset);
}

// Reads the SYS_RIO_BANKn OE register (the output-enable state for every GPIO in 'bank').
static uint32_t
rp1GpioSysRioOeRead(volatile uint32_t *baseP, int bank)
{
    return( rp1GpioRead32(baseP, gpioState.sys_rio[bank], RP1_GPIO_SYS_RIO_REG_OE_OFFSET) );
}

// Atomically disables (input mode) the output-enable bit for GPIO offset 'offset' in bank
// 'bank' via the SYS_RIO OE CLR-alias register. No return value.
static void
rp1GpioSysRioOeClr(volatile uint32_t *baseP, int bank, int offset)
{
    rp1GpioWrite32(baseP, gpioState.sys_rio[bank],
        RP1_GPIO_SYS_RIO_REG_OE_OFFSET + RP1_CLR_OFFSET, 1U << offset);
}

// Atomically enables (output mode) the output-enable bit for GPIO offset 'offset' in bank
// 'bank' via the SYS_RIO OE SET-alias register. No return value.
static void
rp1GpioSysRioOeSet(volatile uint32_t *baseP, int bank, int offset)
{
    rp1GpioWrite32(baseP, gpioState.sys_rio[bank],
        RP1_GPIO_SYS_RIO_REG_OE_OFFSET + RP1_SET_OFFSET, 1U << offset);
}

// Sets the direction of GPIO 'gpio' via the SYS_RIO OE set/clear registers. No return value;
// asserts if 'dir' is neither DIR_INPUT nor DIR_OUTPUT.
static void
rp1SetDir(void *priv, uint32_t gpio, GPIO_DIR_T dir)
{
volatile uint32_t *baseP;
int bank, offset;

    baseP = priv;

    rp1GpioGetBank(gpio, &bank, &offset);

    if( dir == DIR_INPUT )
    {
        rp1GpioSysRioOeClr(baseP, bank, offset);
    }
    else if( dir == DIR_OUTPUT )
    {
        rp1GpioSysRioOeSet(baseP, bank, offset);
    }
    else
    {
        assert(0);
    }
}

// Returns the current direction (DIR_INPUT/DIR_OUTPUT) of GPIO 'gpio' from the SYS_RIO OE
// register.
static GPIO_DIR_T
rp1GetDir(void *priv, unsigned gpio)
{
volatile uint32_t *baseP;
int bank, offset;
GPIO_DIR_T dir;
uint32_t reg;

    baseP = priv;

    rp1GpioGetBank(gpio, &bank, &offset);
    reg = rp1GpioSysRioOeRead(baseP, bank);

    dir = (reg & (1U << offset)) ? DIR_OUTPUT : DIR_INPUT;

    return(dir);
}

// Returns the current function-select value of GPIO 'gpio', decoded from the IO_BANKn CTRL
// register's FSEL field: the RP1_FSEL_SYS_RIO raw value maps to GPIO_FSEL_GPIO,
// RP1_FSEL_NULL maps to GPIO_FSEL_NONE, and any other in-range raw value maps directly to
// the corresponding GPIO_FSEL_T. Returns GPIO_FSEL_MAX if the raw field value is out of
// range.
static GPIO_FSEL_T
rp1GetFsel(void *priv, unsigned gpio)
{
volatile uint32_t *baseP;
int bank, offset;
uint32_t reg;
GPIO_FSEL_T fsel;
RP1_FSEL_T rsel;

    baseP = priv;

    rp1GpioGetBank(gpio, &bank, &offset);
    reg = rp1GpioCtrlRead(baseP, bank, offset);
    rsel = ((reg & RP1_GPIO_CTRL_FSEL_MASK) >> RP1_GPIO_CTRL_FSEL_LSB);

    if( rsel == RP1_FSEL_SYS_RIO )
    {
        fsel = GPIO_FSEL_GPIO;
    }
    else if( rsel == RP1_FSEL_NULL )
    {
        fsel = GPIO_FSEL_NONE;
    }
    else if( rsel < RP1_FSEL_COUNT )
    {
        fsel = (GPIO_FSEL_T)rsel;
    }
    else
    {
        fsel = GPIO_FSEL_MAX;
    }

    return(fsel);
}

// Sets the function-select of GPIO 'gpio' to 'func' via the IO_BANKn CTRL register's FSEL
// field, additionally updating the SYS_RIO direction (for INPUT/OUTPUT requests) and the
// PADS_BANKn input-enable/open-drain bits to match whether 'func' selects a real peripheral
// function or leaves the pin unconnected (RP1_FSEL_NULL). No return value; does nothing if
// 'func' has no RP1 raw-fsel encoding.
static void
rp1SetFsel(void *priv, unsigned gpio, const GPIO_FSEL_T func)
{
volatile uint32_t *baseP;
int bank, offset;
uint32_t ctrlReg;
uint32_t padReg;
uint32_t oldPadReg;
RP1_FSEL_T rsel;

    baseP = priv;

    if( func < (GPIO_FSEL_T)RP1_FSEL_COUNT )
    {
        rsel = (RP1_FSEL_T)func;
    }
    else if( (func == GPIO_FSEL_INPUT) || (func == GPIO_FSEL_OUTPUT) || (func == GPIO_FSEL_GPIO) )
    {
        rsel = RP1_FSEL_SYS_RIO;
    }
    else if( func == GPIO_FSEL_NONE )
    {
        rsel = RP1_FSEL_NULL;
    }
    else
    {
        return;
    }

    rp1GpioGetBank(gpio, &bank, &offset);

    if( func == GPIO_FSEL_INPUT )
    {
        rp1SetDir(priv, gpio, DIR_INPUT);
    }
    else if( func == GPIO_FSEL_OUTPUT )
    {
        rp1SetDir(priv, gpio, DIR_OUTPUT);
    }

    ctrlReg = rp1GpioCtrlRead(baseP, bank, offset) & ~RP1_GPIO_CTRL_FSEL_MASK;
    ctrlReg |= rsel << RP1_GPIO_CTRL_FSEL_LSB;
    rp1GpioCtrlWrite(baseP, bank, offset, ctrlReg);

    padReg = rp1GpioPadsRead(baseP, bank, offset);
    oldPadReg = padReg;

    if( rsel == RP1_FSEL_NULL )
    {
        // Disable input
        padReg &= ~RP1_PADS_IE_SET;
    }
    else
    {
        // Enable input
        padReg |= RP1_PADS_IE_SET;
    }

    if( rsel != RP1_FSEL_NULL )
    {
        // Enable peripheral func output
        padReg &= ~RP1_PADS_OD_SET;
    }
    else
    {
        // Disable peripheral func output
        padReg |= RP1_PADS_OD_SET;
    }

    if( padReg != oldPadReg )
    {
        rp1GpioPadsWrite(baseP, bank, offset, padReg);
    }
}

// Returns the observed input level (0 or 1) of GPIO 'gpio' from the SYS_RIO SYNC_IN register,
// or -1 if the pad's input-enable bit is not set (matches the original tool's behavior of
// treating a disabled input as "no reading available").
static int
rp1GetLevel(void *priv, unsigned gpio)
{
volatile uint32_t *baseP;
int bank, offset;
uint32_t padReg;
uint32_t reg;
int level;

    baseP = priv;

    rp1GpioGetBank(gpio, &bank, &offset);
    padReg = rp1GpioPadsRead(baseP, bank, offset);

    if( !(padReg & RP1_PADS_IE_SET) )
    {
        return(-1);
    }

    reg = rp1GpioSysRioSyncInRead(baseP, bank, offset);
    level = (reg & (1U << offset)) ? 1 : 0;

    return(level);
}

// Drives GPIO 'gpio' high or low via the SYS_RIO OUT set/clear registers. No return value;
// does nothing if 'drv' is neither DRIVE_HIGH nor DRIVE_LOW.
static void
rp1SetDrive(void *priv, unsigned gpio, GPIO_DRIVE_T drv)
{
volatile uint32_t *baseP;
int bank, offset;

    baseP = priv;

    rp1GpioGetBank(gpio, &bank, &offset);

    if( drv == DRIVE_HIGH )
    {
        rp1GpioSysRioOutSet(baseP, bank, offset);
    }
    else if( drv == DRIVE_LOW )
    {
        rp1GpioSysRioOutClr(baseP, bank, offset);
    }
}

// Sets the pull resistor setting of GPIO 'gpio' via the PADS_BANKn PUE/PDE bits. No return
// value; a 'pull' of PULL_NONE (or any other unrecognized value) clears both bits.
static void
rp1SetPull(void *priv, unsigned gpio, GPIO_PULL_T pull)
{
volatile uint32_t *baseP;
uint32_t reg;
int bank, offset;

    baseP = priv;

    rp1GpioGetBank(gpio, &bank, &offset);
    reg = rp1GpioPadsRead(baseP, bank, offset);
    reg &= ~(RP1_PADS_PDE_SET | RP1_PADS_PUE_SET);

    if( pull == PULL_UP )
    {
        reg |= RP1_PADS_PUE_SET;
    }
    else if( pull == PULL_DOWN )
    {
        reg |= RP1_PADS_PDE_SET;
    }

    rp1GpioPadsWrite(baseP, bank, offset, reg);
}

// Returns the current pull resistor setting of GPIO 'gpio' from the PADS_BANKn PUE/PDE bits.
// Returns PULL_NONE if neither bit is set.
static GPIO_PULL_T
rp1GetPull(void *priv, unsigned gpio)
{
volatile uint32_t *baseP;
uint32_t reg;
GPIO_PULL_T pull;
int bank, offset;

    baseP = priv;
    pull = PULL_NONE;

    rp1GpioGetBank(gpio, &bank, &offset);
    reg = rp1GpioPadsRead(baseP, bank, offset);

    if( reg & RP1_PADS_PUE_SET )
    {
        pull = PULL_UP;
    }
    else if( reg & RP1_PADS_PDE_SET )
    {
        pull = PULL_DOWN;
    }

    return(pull);
}

// Returns the current drive level (DRIVE_LOW/DRIVE_HIGH) of GPIO 'gpio' as last written to
// SYS_RIO OUT.
static GPIO_DRIVE_T
rp1GetDrive(void *priv, unsigned gpio)
{
volatile uint32_t *baseP;
uint32_t reg;
int bank, offset;

    baseP = priv;

    rp1GpioGetBank(gpio, &bank, &offset);
    reg = rp1GpioSysRioOutRead(baseP, bank, offset);

    return( (reg & (1U << offset)) ? DRIVE_HIGH : DRIVE_LOW );
}

// Returns a human-readable "GPIOn" name for GPIO 'gpio', formatted into a static buffer
// (matches the original tool's behavior -- not reentrant/thread-safe). Returns NULL if
// 'gpio' is out of range.
static const char *
rp1GetName(void *priv, unsigned gpio)
{
static char nameBuf[16];

    UNUSED(priv);

    if( gpio >= RP1_NUM_GPIOS )
    {
        return(NULL);
    }

    sprintf(nameBuf, "GPIO%d", gpio);
    return(nameBuf);
}

// Returns a human-readable name for what function-select value 'fsel' means on RP1 GPIO
// 'gpio' specifically (looked up in rp1GpioFselNames for alternate functions), or NULL if
// 'fsel' is not a recognized function-select value.
static const char *
rp1GetFselName(void *priv, unsigned gpio, GPIO_FSEL_T fsel)
{
const char *nameP;

    nameP = NULL;
    UNUSED(priv);

    switch( fsel )
    {
    case GPIO_FSEL_GPIO:
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

    case GPIO_FSEL_FUNC0:
    case GPIO_FSEL_FUNC1:
    case GPIO_FSEL_FUNC2:
    case GPIO_FSEL_FUNC3:
    case GPIO_FSEL_FUNC4:
    case GPIO_FSEL_FUNC5:
    case GPIO_FSEL_FUNC6:
    case GPIO_FSEL_FUNC7:
    case GPIO_FSEL_FUNC8:
        if( gpio < RP1_NUM_GPIOS )
        {
            nameP = rp1GpioFselNames[gpio][fsel - GPIO_FSEL_FUNC0];
            if( !nameP )
            {
                nameP = "-";
            }
        }
        break;

    default:
        return(NULL);
    }

    return(nameP);
}

// Allocates a per-instance state block for an RP1 chip instance. This chip has no
// per-instance state beyond the GPIO_CHIP_T itself, so it just hands 'chip' back as the
// opaque 'priv' pointer other callbacks receive. Never returns NULL (device-tree node
// 'dtnode' is unused/ignored).
static void *
rp1CreateInstance(const GPIO_CHIP_T *chip, const char *dtnode)
{
    UNUSED(dtnode);
    return( (void *)chip );
}

// Returns the number of GPIOs this chip instance provides (fixed at RP1_NUM_GPIOS).
static int
rp1GpioCount(void *priv)
{
    UNUSED(priv);
    return(RP1_NUM_GPIOS);
}

// Completes instance setup once the physical register window has been mmap()'d to 'base';
// this chip just uses the mapped base pointer directly as its 'priv' value for every
// subsequent callback. Never returns NULL.
static void *
rp1ProbeInstance(void *priv, volatile uint32_t *base)
{
    UNUSED(priv);
    return( (void *)base );
}

static const GPIO_CHIP_INTERFACE_T rp1GpioInterface =
{
    .gpio_create_instance = rp1CreateInstance,
    .gpio_count = rp1GpioCount,
    .gpio_probe_instance = rp1ProbeInstance,
    .gpio_get_fsel = rp1GetFsel,
    .gpio_set_fsel = rp1SetFsel,
    .gpio_set_drive = rp1SetDrive,
    .gpio_set_dir = rp1SetDir,
    .gpio_get_dir = rp1GetDir,
    .gpio_get_level = rp1GetLevel,
    .gpio_get_drive = rp1GetDrive,
    .gpio_get_pull = rp1GetPull,
    .gpio_set_pull = rp1SetPull,
    .gpio_get_name = rp1GetName,
    .gpio_get_fsel_name = rp1GetFselName,
};

DECLARE_GPIO_CHIP(rp1, "raspberrypi,rp1-gpio", &rp1GpioInterface, 0x30000, 0);
