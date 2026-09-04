/* SPDX-License-Identifier: MIT
 *
 * Board-specific bootloader config + init for the Apex Pro Mini WL (nRF52833).
 *
 * board_init2() raises the three board-control lines required before USB
 * enumeration:
 *
 *     P0.25, P0.23, P0.19  -> cfg_output + drive high
 *
 * P0.25 enables U10 in the USB data path. P0.23 and P0.19 enable the RGB
 * array and driver. USB does not enumerate if the data-path switch is off.
 *
 * UICR_REGOUT0_VALUE is not defined because the board uses VDD and leaves
 * REGOUT0 erased. Defining it would cause an unnecessary UICR rewrite/reset.
 */
#include <string.h>

#include "boards.h"
#include "board.h"
#include "uf2/configkeys.h"

__attribute__((used, section(".bootloaderConfig")))
const uint32_t bootloaderConfig[] =
{
  /* CF2 START */
  CFG_MAGIC0, CFG_MAGIC1,                       // magic
  5, 100,                                       // used entries, total entries

  204, 0x80000,                                 // FLASH_BYTES  (nRF52833 512 KB)
  205, 0x20000,                                 // RAM_BYTES    (nRF52833 128 KB)
  208, (USB_DESC_VID << 16) | USB_DESC_UF2_PID, // BOOTLOADER_BOARD_ID = USB VID+PID
  209, 0x621e937a,                              // UF2_FAMILY
  210, 0x20,                                    // PINS_PORT_SIZE = PA_32

  0, 0, 0, 0, 0, 0, 0, 0
  /* CF2 END */
};

/* The USB / STM32-scanner power rails the stock loader raises before USB. */
static void apex_rails_up(void)
{
  /* Order mirrors the stock loader (P0.25, then the LED-array/driver pair). */
  nrf_gpio_cfg_output(PINNUM(0, 25));
  nrf_gpio_pin_write(PINNUM(0, 25), 1);

  nrf_gpio_cfg_output(PINNUM(0, 23));
  nrf_gpio_pin_write(PINNUM(0, 23), 1);

  nrf_gpio_cfg_output(PINNUM(0, 19));
  nrf_gpio_pin_write(PINNUM(0, 19), 1);
}

void board_init2(void)
{
  apex_rails_up();
}

#ifdef BOARD_HAS_RGB_DFU
/*------------------------------------------------------------------*/
/* Drive the IS31FL3743B over SPIM2 for the visible DFU state. Transfers use a
 * bounded wait so a peripheral fault cannot hold the bootloader indefinitely.
 *------------------------------------------------------------------*/
#define APEX_RGB_CS_PIN     PINNUM(0, 11)
#define APEX_RGB_PSEL_SCK   (9u | (1u << 5))   /* P1.09 */
#define APEX_RGB_PSEL_MOSI  (8u | (1u << 5))   /* P1.08 */
#define APEX_RGB_LEDS       66u
#define APEX_RGB_CHANNELS   (APEX_RGB_LEDS * 3u)
#define APEX_RGB_CMD_PWM    0x50u
#define APEX_RGB_CMD_SCALE  0x51u
#define APEX_RGB_CMD_FUNC   0x52u

static bool    apex_rgb_ready = false;
static uint8_t apex_rgb_frame[2u + APEX_RGB_CHANNELS]; /* RAM: EasyDMA source */

static void apex_spim2_write(const uint8_t *buf, uint32_t len)
{
  nrf_gpio_pin_write(APEX_RGB_CS_PIN, 0);   /* CS low (active) */

  NRF_SPIM2->TXD.PTR    = (uint32_t) buf;
  NRF_SPIM2->TXD.MAXCNT = len;
  NRF_SPIM2->RXD.PTR    = 0u;
  NRF_SPIM2->RXD.MAXCNT = 0u;
  NRF_SPIM2->EVENTS_END = 0u;
  (void) NRF_SPIM2->EVENTS_END;
  NRF_SPIM2->TASKS_START = 1u;

  /* A 200-byte transfer takes about 0.4 ms at 4 Mbit/s. */
  for (volatile uint32_t i = 0u; i < 4000000u; i++) {
    if (NRF_SPIM2->EVENTS_END) break;
  }

  nrf_gpio_pin_write(APEX_RGB_CS_PIN, 1);   /* CS high (idle) */
}

static void apex_rgb_func(uint8_t reg, uint8_t val)
{
  uint8_t f[3] = { APEX_RGB_CMD_FUNC, reg, val };
  apex_spim2_write(f, sizeof(f));
}

static void apex_rgb_init(void)
{
  apex_rails_up();  /* the IS31 loses all state while P0.19 is low */

  nrf_gpio_cfg_output(APEX_RGB_CS_PIN);
  nrf_gpio_pin_write(APEX_RGB_CS_PIN, 1);

  NRF_SPIM2->PSEL.SCK  = APEX_RGB_PSEL_SCK;
  NRF_SPIM2->PSEL.MOSI = APEX_RGB_PSEL_MOSI;
  NRF_SPIM2->PSEL.MISO = 0xFFFFFFFFu; /* write-only device */
  NRF_SPIM2->PSEL.CSN  = 0xFFFFFFFFu; /* CS is bit-banged */
  NRF_SPIM2->FREQUENCY = 0x40000000u; /* M4 (4 Mbit/s), as the loader leaves it */
  NRF_SPIM2->CONFIG    = 0u;          /* mode 0, MSB first */
  NRF_SPIM2->ORC       = 0u;
  NRF_SPIM2->INTENCLR  = 0xFFFFFFFFu;
  NRF_SPIM2->ENABLE    = 7u;
  __DSB();

  /* Load scaling before leaving shutdown, then select a moderate current. */
  apex_rgb_frame[0] = APEX_RGB_CMD_SCALE;
  apex_rgb_frame[1] = 0x01u;
  memset(&apex_rgb_frame[2], 0xFFu, APEX_RGB_CHANNELS);
  apex_spim2_write(apex_rgb_frame, 2u + APEX_RGB_CHANNELS);

  apex_rgb_func(0x02u, 0x33u); /* PULL   */
  apex_rgb_func(0x00u, 0x09u); /* CONFIG = normal run */
  apex_rgb_func(0x01u, 0x40u); /* GLOBAL CURRENT ~1/4 */

  apex_rgb_ready = true;
}

/* Called for DFU states. rgb is 0xRRGGBB; the device register order is B,G,R. */
void board_rgb_dfu(uint32_t rgb)
{
  uint8_t r = ((rgb >> 16) & 0xFFu) ? 0xFFu : 0u;
  uint8_t g = ((rgb >>  8) & 0xFFu) ? 0xFFu : 0u;
  uint8_t b = ((rgb >>  0) & 0xFFu) ? 0xFFu : 0u;

  if (!apex_rgb_ready) apex_rgb_init();

  apex_rgb_frame[0] = APEX_RGB_CMD_PWM;
  apex_rgb_frame[1] = 0x01u;
  for (uint32_t i = 0u; i < APEX_RGB_LEDS; i++) {
    uint8_t *px = &apex_rgb_frame[2u + i * 3u];
    px[0] = b; px[1] = g; px[2] = r;
  }
  apex_spim2_write(apex_rgb_frame, 2u + APEX_RGB_CHANNELS);
}
#endif /* BOARD_HAS_RGB_DFU */

/* Keep board-control lines asserted while transferring control to the app. */
void board_teardown2(void)
{
  apex_rails_up();
  /* POWER/USBD/NVIC remain untouched until the application's early USB init. */
}
