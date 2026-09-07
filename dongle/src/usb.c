/* SPDX-License-Identifier: MIT */
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "gamepad.h"
LOG_MODULE_REGISTER(receiver_usb, LOG_LEVEL_WRN);

/* Local development IDs, matching the existing receiver allocation. */
USBD_DEVICE_DEFINE(receiver_usb, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x1d50, 0x6171);
USBD_DESC_LANG_DEFINE(receiver_lang);
USBD_DESC_MANUFACTURER_DEFINE(receiver_mfr, "Apex community firmware");
USBD_DESC_PRODUCT_DEFINE(receiver_product, "Apex wireless receiver");
USBD_DESC_SERIAL_NUMBER_DEFINE(receiver_serial);
USBD_DESC_CONFIG_DEFINE(receiver_cfg_desc, "Receiver");
USBD_CONFIGURATION_DEFINE(receiver_cfg, 0, 50, &receiver_cfg_desc);

static atomic_t gamepad_target, started;
static bool gamepad_present;

static int register_classes(bool gamepad)
{
    const char *blocklist[] = { "hid_1", NULL };
    return usbd_register_all_classes(&receiver_usb, USBD_SPEED_FS, 1,
                                     gamepad ? NULL : blocklist);
}

static int bring_up(bool gamepad)
{
    /* usbd_shutdown removes classes and string descriptors, but retains the
     * configuration node. Re-add its strings before registering classes. */
    struct usbd_desc_node *descriptors[] = {
        &receiver_lang, &receiver_mfr, &receiver_product, &receiver_serial,
    };
    int rc;
    for (size_t i = 0; i < ARRAY_SIZE(descriptors); ++i) {
        rc = usbd_add_descriptor(&receiver_usb, descriptors[i]);
        if (rc) return rc;
    }
    rc = usbd_add_configuration(&receiver_usb, USBD_SPEED_FS, &receiver_cfg);
    if (!rc) rc = register_classes(gamepad);
    if (!rc) rc = usbd_init(&receiver_usb);
    if (!rc) rc = usbd_enable(&receiver_usb);
    return rc;
}

static void reconfigure(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!atomic_get(&started)) return;
    bool requested = atomic_get(&gamepad_target) != 0;
    if (requested == gamepad_present) return;
    (void)usbd_disable(&receiver_usb);
    (void)usbd_shutdown(&receiver_usb);
    int rc = bring_up(requested);
    if (!rc) {
        gamepad_present = requested;
    } else {
        LOG_ERR("Cannot change gamepad interface: %d", rc);
        (void)usbd_disable(&receiver_usb);
        (void)usbd_shutdown(&receiver_usb);
        if (bring_up(gamepad_present)) LOG_ERR("Cannot restore USB interfaces");
    }
    if (requested != (atomic_get(&gamepad_target) != 0)) k_work_submit(work);
}
static K_WORK_DEFINE(class_work, reconfigure);

void receiver_usb_gamepad(bool enabled)
{
    if (atomic_set(&gamepad_target, enabled) != enabled) k_work_submit(&class_work);
}

static void usb_event(struct usbd_context *ctx, const struct usbd_msg *msg)
{
    if (msg->type == USBD_MSG_VBUS_READY) {
        (void)usbd_enable(ctx);
    } else if (msg->type == USBD_MSG_VBUS_REMOVED) {
        (void)usbd_disable(ctx);
    }
}

int receiver_usb_init(void)
{
    int rc;
    usbd_device_set_code_triple(&receiver_usb, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 2, 1);
    rc = usbd_msg_register_cb(&receiver_usb, usb_event);
    if (rc) return rc;
    rc = bring_up(false);
    if (!rc) {
        atomic_set(&started, 1);
        k_work_submit(&class_work);
    }
    return rc;
}
