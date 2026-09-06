/* SPDX-License-Identifier: MIT */
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>

/* Local development IDs, matching the existing receiver allocation. */
USBD_DEVICE_DEFINE(receiver_usb, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x1d50, 0x6171);
USBD_DESC_LANG_DEFINE(receiver_lang);
USBD_DESC_MANUFACTURER_DEFINE(receiver_mfr, "Apex community firmware");
USBD_DESC_PRODUCT_DEFINE(receiver_product, "Apex wireless receiver");
USBD_DESC_SERIAL_NUMBER_DEFINE(receiver_serial);
USBD_DESC_CONFIG_DEFINE(receiver_cfg_desc, "Receiver");
USBD_CONFIGURATION_DEFINE(receiver_cfg, 0, 50, &receiver_cfg_desc);

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
    struct usbd_desc_node *descriptors[] = {
        &receiver_lang, &receiver_mfr, &receiver_product, &receiver_serial,
    };
    for (size_t i = 0; i < ARRAY_SIZE(descriptors); ++i) {
        rc = usbd_add_descriptor(&receiver_usb, descriptors[i]);
        if (rc) return rc;
    }
    rc = usbd_add_configuration(&receiver_usb, USBD_SPEED_FS, &receiver_cfg);
    if (rc) return rc;
    rc = usbd_register_all_classes(&receiver_usb, USBD_SPEED_FS, 1, NULL);
    if (rc) return rc;
    usbd_device_set_code_triple(&receiver_usb, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 2, 1);
    rc = usbd_msg_register_cb(&receiver_usb, usb_event);
    if (rc) return rc;
    rc = usbd_init(&receiver_usb);
    if (rc) return rc;
    return usbd_enable(&receiver_usb);
}
