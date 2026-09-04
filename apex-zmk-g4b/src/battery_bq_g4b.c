/* SPDX-License-Identifier: MIT
 *
 * The BQ25895 as a Zephyr sensor, so ZMK's battery reporting can publish it
 * over the standard BLE Battery Service.
 *
 * A shim, in the same spirit as led_strip_g4b.c: ZMK wants a device that
 * answers sensor_sample_fetch_chan() and sensor_channel_get(), and does not
 * care how that device reaches hardware. So the sensor API is implemented over
 * the direct-register TWI path in twi_g4b.c rather than over Zephyr's I2C
 * stack. That keeps CONFIG_I2C / SPI / GPIO / PINCTRL off, which verify_g4b.py
 * enforces and which the whole payload is built around.
 *
 * WHY FETCH IS DEFERRED TO ITS OWN CALL. A conversion takes tens of
 * milliseconds and this runs on ZMK's low-priority work queue, not the scan
 * loop, so blocking here is fine - but it must never happen on the g4b thread.
 * Nothing in this file is called from there.
 */

#define DT_DRV_COMPAT apex_battery_bq25895

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include "twi_g4b.h"

struct battery_bq_data {
    uint32_t mv;
    uint8_t percent;
};

static int battery_bq_sample_fetch(const struct device *dev,
                                   enum sensor_channel chan)
{
    struct battery_bq_data *data = dev->data;
    uint32_t mv;

    if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE &&
        chan != SENSOR_CHAN_VOLTAGE) {
        return -ENOTSUP;
    }

    mv = g4b_bq_sample_mv();
    if (mv == 0u) {
        /* The charger did not answer, or the conversion did not finish. Report
         * the failure rather than publishing a zero: ZMK would otherwise show a
         * flat battery, which is worse than showing the last good value.
         */
        return -EIO;
    }

    data->mv = mv;
    data->percent = (uint8_t)g4b_bq_percent(mv);

    /* Run the storage-band charge controller off the same reading. ZMK's battery
     * reporting fetches this every CONFIG_ZMK_BATTERY_REPORT_INTERVAL seconds, so
     * the controller is serviced without a thread of its own. No-op unless
     * CONFIG_APEX_G4B_CHARGE_STORAGE. */
    g4b_bq_storage_tick(mv);
    return 0;
}

static int battery_bq_channel_get(const struct device *dev,
                                  enum sensor_channel chan,
                                  struct sensor_value *val)
{
    struct battery_bq_data *data = dev->data;

    switch (chan) {
    case SENSOR_CHAN_GAUGE_STATE_OF_CHARGE:
        val->val1 = (int32_t)data->percent;
        val->val2 = 0;
        return 0;
    case SENSOR_CHAN_VOLTAGE:
        /* Volts and microvolts, as the sensor API expects. */
        val->val1 = (int32_t)(data->mv / 1000u);
        val->val2 = (int32_t)((data->mv % 1000u) * 1000u);
        return 0;
    default:
        return -ENOTSUP;
    }
}

static const struct sensor_driver_api battery_bq_api = {
    .sample_fetch = battery_bq_sample_fetch,
    .channel_get = battery_bq_channel_get,
};

static int battery_bq_init(const struct device *dev)
{
    ARG_UNUSED(dev);

    /* Nothing to bring up: twi_g4b.c enables and disables the peripheral around
     * every transfer, so there is no persistent state to establish here. Charge
     * configuration is deliberately NOT done from this init - it belongs on the
     * g4b thread next to the rest of the bring-up, so that a gauge that fails to
     * probe cannot leave the charger half-configured.
     */
    return 0;
}

static struct battery_bq_data battery_bq_data_inst;

DEVICE_DT_INST_DEFINE(0, battery_bq_init, NULL, &battery_bq_data_inst, NULL,
                      POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,
                      &battery_bq_api);
