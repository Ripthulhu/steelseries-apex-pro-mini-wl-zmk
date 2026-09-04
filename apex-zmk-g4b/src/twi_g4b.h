/* SPDX-License-Identifier: MIT */
#ifndef APEX_G4B_TWI_H
#define APEX_G4B_TWI_H

#include <stdbool.h>
#include <stdint.h>

/* BQ25895 battery charger access over TWI1.
 *
 * Register writes are private to twi_g4b.c and restricted to REG02, REG03,
 * REG04, REG06, and REG07. Callers use named operations with fixed values or
 * masked read-modify-write updates; there is no exported write(reg, value)
 * function. REG00, including EN_HIZ, is unreachable from this module.
 *
 * Per SLUSC88C 8.3.1, any register write moves the BQ25895 out of autonomous
 * mode and starts a 40-second I2C watchdog. REG03 controls charging, REG04 sets
 * charge current, REG06 sets charge voltage, and REG07 carries the watchdog and
 * safety timer. The charge constants are checked at build time.
 *
 * Setting the register POINTER, which every read does, is part of the I2C read
 * protocol and not a register write - no register's value changes.
 *
 * Pin assignment comes from the stock nrfx_twi configuration at
 * flash at vaddr 0x00044BF8:
 *     10 00 00 00   scl  = P0.16
 *     11 00 00 00   sda  = P0.17
 *     00 00 68 06   frequency = 0x06680000 (400 kHz)
 *     07 01 00 00   irq priority 7, hold_bus_uninit
 * PSEL.SCL is base+0x508 and PSEL.SDA base+0x50C, so which pin is which is
 * fixed by the nRF52833 register map rather than by guessing struct order.
 * Two independent readers hand-decoded the Thumb-2 at 0x33380 to rule out the
 * one dangerous confusion here: GPIO OUTSET/OUTCLR live at 0x50000508 and
 * 0x5000050C, one bit of address away from the TWI PSEL pair, and believing
 * that would have had us driving the wrong pins.
 *
 * This driver uses 100 kHz rather than stock's 400 kHz. These transactions are
 * not rate-sensitive, and the lower rate tolerates weaker pull-ups.
 */

/* Read-only status registers. */
#define G4B_BQ_REG_STATUS   0x0Bu /* VBUS_STAT, CHRG_STAT, PG_STAT, VSYS_STAT */
#define G4B_BQ_REG_FAULT    0x0Cu /* WATCHDOG_FAULT, CHRG_FAULT, BAT_FAULT... */
#define G4B_BQ_REG_BATV     0x0Eu /* THERM_STAT + battery voltage, 20 mV steps */
#define G4B_BQ_REG_VBUSV    0x11u /* VBUS_GD + VBUS voltage */

/* Result of one attempted read. */
struct g4b_twi_result {
    uint8_t value;   /* the register byte, valid only when ok is true */
    uint8_t errorsrc; /* TWI ERRORSRC: bit1 ANACK (no device), bit2 DNACK */
    bool ok;
};

/* Configure TWI1 and read one register from the charger.
 *
 * Enables the peripheral, reads, and disables it again, so the bus is left
 * exactly as it was found. Every wait is bounded; a missing or unresponsive
 * device returns ok=false with the NACK bit set rather than hanging.
 *
 * Returns a zeroed result with ok=false if the read did not complete.
 */
struct g4b_twi_result g4b_bq_read(uint8_t reg);

/* Trigger ONE conversion, then read the battery voltage. Returns millivolts,
 * or 0 if the conversion or either transfer failed.
 *
 * This is the ONLY write this module can perform, and it is a dedicated
 * function rather than a general write for the reason above: a general write
 * would put REG04 (charge current), REG06 (charge voltage limit) and REG07
 * (safety timer) one argument away from a conversion trigger. This touches
 * REG02 bit 7 and nothing else, read-modify-write, so ICO_EN / HVDCP_EN /
 * AUTO_DPDM_EN keep whatever they had - a blind write there would break USB
 * source detection.
 *
 * WHY A WRITE IS NEEDED AT ALL: REG0E reads 0 until the ADC has run, and it
 * does not run by itself. Measured on hardware, REG0E = 0x00 with the charger
 * in its power-on state. There is no read-only route to a battery voltage.
 *
 * WHAT IT COSTS: the write starts the charger's 40 s I2C watchdog, and when
 * that lapses REG00-REG07 return to power-on defaults - which is exactly where
 * this board already sits (REG0C read back 0x80, WATCHDOG_FAULT). So a one-shot
 * conversion ends where it started and leaves no lasting change.
 *
 * Deliberately NOT stock's approach: stock sets REG02 bit 6 (CONV_RATE) once at
 * boot and leaves the ADC converting at 1 Hz forever, holding the REGN LDO up
 * and costing quiescent current on a device running off the cell it measures.
 */
uint32_t g4b_bq_sample_mv(void);

/* Millivolts to percent, using the vendor curve for this exact pack, scaled to
 * whatever charge ceiling is actually in force. See the note in twi_g4b.c.
 */
uint32_t g4b_bq_percent(uint32_t mv);

/* Charge current in milliamps, from the last conversion. Call g4b_bq_sample_mv()
 * first to trigger one. Reads 0 both when nothing is flowing and when REG12 is
 * not applicable (boost, charge disabled, battery only), so gate a display on
 * g4b_bq_is_charging() rather than on this being non-zero.
 */
uint32_t g4b_bq_charge_ma(void);

/* True while current is actually flowing into the cell - CHRG_STAT pre-charge
 * or fast charge. Termination-done reads false: the pack is full, not charging.
 */
bool g4b_bq_is_charging(void);

/* Take ownership of charging configuration, once, at boot.
 *
 * Disables the charger's I2C watchdog FIRST (read-modify-write on REG07,
 * touching only the WATCHDOG field), then sets the charge voltage limit to
 * 4.096 V and the charge current to 1472 mA. Order matters: any write starts a
 * 40 s watchdog, and letting it lapse reverts REG00-REG07 to power-on defaults,
 * so configuring first and disarming second would undo itself after 40 seconds.
 *
 * 4.096 V is a deliberate longevity ceiling, well under the pack's 4.400 V rated
 * maximum (Fuji 4867A0, 5870 mAh, 3.85 V nominal) and under the 4.208 V power-on
 * default too. A cell in an always-plugged-in keyboard sits at its charge
 * voltage for months on end, and parking a high-voltage cell near its top is
 * what swells it; charging only to 4.096 V trades some runtime for a cell that
 * lasts. See G4B_BQ_VREG_4096MV in twi_g4b.c.
 *
 * Returns false if any step failed, in which case the charger is left wherever
 * it got to - which is safe, because every intermediate state is either the
 * power-on default or a lower limit than the target.
 */
bool g4b_bq_configure_charge(void);

/* --- Charge on/off control + storage-band controller (see twi_g4b.c) ---------
 *
 * Enable or disable battery charging via REG03 CHG_CONFIG (masked read-modify-
 * write of that one bit). Disabled = PASSTHROUGH: the system keeps running from
 * VBUS and the cell is left idle. Returns false on I2C failure.
 */
bool g4b_bq_set_charging(bool enable);

/* Current CHG_CONFIG state (true = charging enabled). */
bool g4b_bq_charging_enabled(void);

/* True when CHRG_STAT reports charge termination (the pack topped to the VREG
 * cap). Distinct from g4b_bq_is_charging(), which is true only while current
 * flows. */
bool g4b_bq_charge_terminated(void);

/* True when the input is present and good (REG0B PG_STAT) - i.e. plugged in. */
bool g4b_bq_power_good(void);

/* Storage-band controller tick. Call periodically with a fresh battery reading
 * (the ZMK battery fetch already samples every 60 s). When plugged in it holds
 * the pack near CONFIG_APEX_G4B_CHARGE_STOP_PCT and resumes charging only below
 * CONFIG_APEX_G4B_CHARGE_RESUME_PCT, running passthrough in between. A no-op
 * unless CONFIG_APEX_G4B_CHARGE_STORAGE. */
void g4b_bq_storage_tick(uint32_t mv);

#endif /* APEX_G4B_TWI_H */
