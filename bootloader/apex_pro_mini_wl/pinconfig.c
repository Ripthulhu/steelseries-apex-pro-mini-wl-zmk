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
  apex_rgb_func(0x01u, 0xFFu); /* GLOBAL CURRENT = max (100% brightness) */

  apex_rgb_ready = true;
}

/* Shared DFU-indicator state (used by the flood, the breathe and the write bar). */
static volatile bool apex_dfu_idle = false; /* breathe armed (set on USB mount) */
static volatile bool apex_rgb_lock = false; /* guards SPIM2/frame vs the led_tick ISR */
static uint32_t apex_dfu_last_ms = 0u;

/* Flood every key one colour (rgb 0xRRGGBB; device order B,G,R). Green = success,
 * dark = ejected. Disarms the idle breathe. */
void board_rgb_dfu(uint32_t rgb)
{
  uint8_t r = ((rgb >> 16) & 0xFFu) ? 0xFFu : 0u;
  uint8_t g = ((rgb >>  8) & 0xFFu) ? 0xFFu : 0u;
  uint8_t b = ((rgb >>  0) & 0xFFu) ? 0xFFu : 0u;

  if (!apex_rgb_ready) apex_rgb_init();
  apex_dfu_idle = false;

  apex_rgb_frame[0] = APEX_RGB_CMD_PWM;
  apex_rgb_frame[1] = 0x01u;
  for (uint32_t i = 0u; i < APEX_RGB_LEDS; i++) {
    uint8_t *px = &apex_rgb_frame[2u + i * 3u];
    px[0] = b; px[1] = g; px[2] = r;
  }
  apex_spim2_write(apex_rgb_frame, 2u + APEX_RGB_CHANNELS);
}

/* DFU visuals: a breathing red idle background with green D/F/U keys, and a
 * left->right red write bar, on the per-LED framebuffer (order B,G,R). */

/* LED index -> horizontal position (0 = left .. 255 = right), mirror of
 * g4b_led_x[] in the app's rgb_map_g4b.c. */
static const uint8_t apex_led_x[APEX_RGB_LEDS] = {
    0u,   0u,   0u,   0u,   0u,   208u, 19u,  28u,  33u,  28u,  24u,
    227u, 38u,  47u,  52u,  42u,  47u,  231u, 57u,  66u,  71u,  61u,
    123u, 246u, 76u,  85u,  90u,  80u,  71u,  217u, 94u,  104u, 109u,
    99u,  71u,  236u, 113u, 123u, 128u, 118u, 198u, 255u, 132u, 142u,
    146u, 137u, 222u, 222u, 151u, 161u, 165u, 156u, 234u, 241u, 170u,
    179u, 184u, 175u, 246u, 250u, 189u, 198u, 203u, 194u, 212u, 246u,
};
/* LED 52 (Fn/SteelSeries key) overridden to 234: stock g4b_led_x places it at 175
 * (just past the spacebar), which lit it early in the write bar. Bootloader-only. */

/* DFU idle indicator: breathing red with the D/F/U keycaps solid green (their
 * legends read "DFU"). Animated from led_tick() (SysTick, lowest IRQ priority, so
 * the SPIM write cannot stall USB); glyph() arms it, the one-shot writers disarm
 * it. LED indices via g4b_led_hid[]: D=0x07->20, F=0x09->26, U=0x18->43. */
#define APEX_DFU_KEY_D      20u
#define APEX_DFU_KEY_F      26u
#define APEX_DFU_KEY_U      43u
#define APEX_DFU_BREATHE_MS 2600u /* full fade-in + fade-out period */
#define APEX_DFU_RED_MIN    6u
#define APEX_DFU_RED_MAX    255u

static void apex_rgb_begin(void)
{
  apex_rgb_frame[0] = APEX_RGB_CMD_PWM;
  apex_rgb_frame[1] = 0x01u;
  memset(&apex_rgb_frame[2], 0u, APEX_RGB_CHANNELS);
}

static void apex_rgb_set(uint8_t led, uint8_t r, uint8_t g, uint8_t b)
{
  if (led >= APEX_RGB_LEDS) return;
  uint8_t *px = &apex_rgb_frame[2u + (uint32_t)led * 3u];
  px[0] = b; px[1] = g; px[2] = r; /* device order B,G,R */
}

/* One breathe frame: every key red at `red`, D/F/U solid green over the top. */
static void apex_dfu_draw(uint8_t red)
{
  apex_rgb_begin();
  for (uint32_t i = 0u; i < APEX_RGB_LEDS; i++) apex_rgb_set((uint8_t)i, red, 0u, 0u);
  apex_rgb_set(APEX_DFU_KEY_D, 0u, 0xFFu, 0u);
  apex_rgb_set(APEX_DFU_KEY_F, 0u, 0xFFu, 0u);
  apex_rgb_set(APEX_DFU_KEY_U, 0u, 0xFFu, 0u);
  apex_spim2_write(apex_rgb_frame, 2u + APEX_RGB_CHANNELS);
}

/* Arm the breathing DFU idle indicator (from led_state on USB mount). rgb is
 * ignored; colours are fixed. */
void board_rgb_dfu_glyph(uint32_t rgb)
{
  (void)rgb;
  if (!apex_rgb_ready) apex_rgb_init();
  apex_rgb_lock = true;
  apex_dfu_draw(APEX_DFU_RED_MIN);
  apex_rgb_lock = false;
  apex_dfu_last_ms = 0u;
  apex_dfu_idle = true;
}

/* Animate the breathe; called every ms from led_tick() (SysTick ISR). Throttled
 * to ~30 fps and a no-op unless the idle indicator is armed. */
void board_rgb_dfu_tick(uint32_t millis)
{
  if (!apex_dfu_idle || apex_rgb_lock || !apex_rgb_ready) return;
  if ((uint32_t)(millis - apex_dfu_last_ms) < 33u) return;
  apex_dfu_last_ms = millis;

  uint32_t phase = millis % APEX_DFU_BREATHE_MS;
  uint32_t half = APEX_DFU_BREATHE_MS / 2u;
  uint32_t tri = (phase < half) ? phase : (APEX_DFU_BREATHE_MS - phase); /* 0..half */
  uint32_t red = APEX_DFU_RED_MIN + (tri * (APEX_DFU_RED_MAX - APEX_DFU_RED_MIN)) / half;

  apex_rgb_lock = true;
  apex_dfu_draw((uint8_t)red);
  apex_rgb_lock = false;
}

/* Write progress: red bar filling left->right as num/den advances; a full green
 * board when complete. Called once per written block, throttled to redraw only
 * when the 8-bit fill level changes. Disarms the idle breathe. */
void board_rgb_progress(uint32_t num, uint32_t den)
{
  static uint8_t last_level = 0xFEu; /* != any first level, forces one draw */
  uint32_t level;

  apex_dfu_idle = false; /* stop the breathe; the bar owns the frame now */

  if (den == 0u) {
    level = 0u;
  } else if (num >= den) {
    level = 255u;
  } else {
    level = (num * 255u) / den;
  }

  if ((uint8_t)level == last_level) return;
  last_level = (uint8_t)level;

  if (!apex_rgb_ready) apex_rgb_init();

  apex_rgb_lock = true;
  apex_rgb_begin();
  if (level >= 255u) {
    for (uint32_t i = 0u; i < APEX_RGB_LEDS; i++) apex_rgb_set((uint8_t)i, 0u, 0xFFu, 0u);
  } else {
    for (uint32_t i = 0u; i < APEX_RGB_LEDS; i++) {
      if (apex_led_x[i] <= (uint8_t)level) apex_rgb_set((uint8_t)i, 0xFFu, 0u, 0u);
    }
  }
  apex_spim2_write(apex_rgb_frame, 2u + APEX_RGB_CHANNELS);
  if (level >= 255u) {
    /* Hold the green success flash ~1 s; the board resets into the app right after
     * the last block otherwise, leaving it on screen for only a few ms. */
    NRFX_DELAY_MS(1000);
  }
  apex_rgb_lock = false;
}
#endif /* BOARD_HAS_RGB_DFU */

/* Keep board-control lines asserted while transferring control to the app. */
void board_teardown2(void)
{
  apex_rails_up();
  /* POWER/USBD/NVIC remain untouched until the application's early USB init. */
}
