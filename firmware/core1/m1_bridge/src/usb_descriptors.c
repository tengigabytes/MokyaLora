/*
 * usb_descriptors.c — MokyaLora Phase 2 M1.1-B Core 1 m1_bridge
 *
 * Single CDC interface with IAD. VID/PID is a temporary Raspberry Pi Pico 2
 * value (0x2E8A:0x000F) for development — this squats on Raspberry Pi's
 * official RP2350 reference PID, which is acceptable for prototyping per
 * Raspberry Pi's unofficial tolerance, but must be replaced before Rev B
 * production by a PID allocated via the raspberrypi/usb-pid GitHub registry.
 *
 * Product string "MokyaLora CDC" lets Meshtastic CLI's --port argument
 * identify the right COM port visually while we're running tests.
 *
 * Serial number derived from the RP2350's unique board ID so each unit
 * enumerates as a distinct COM port on the host.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include "tusb.h"
#include "pico/unique_id.h"

#define USB_VID   0x2E8Au
#define USB_PID   0x000Fu   /* TEMPORARY — replace before Rev B */
#define USB_BCD   0x0200u

/* ── Device descriptor ──────────────────────────────────────────────────── */
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,

    /* IAD required for CDC class per USB-IF. */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/* ── Configuration descriptor ───────────────────────────────────────────── */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_fs_configuration;
}

/* ── String descriptors ─────────────────────────────────────────────────── */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_ITF,
};

static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, /* 0: English (US)   */
    "tengigabytes",             /* 1: Manufacturer   */
    "MokyaLora CDC",            /* 2: Product        */
    NULL,                       /* 3: Serial (filled from unique board ID) */
    "MokyaLora Bridge",         /* 4: CDC interface  */
};

static uint16_t _desc_str[32 + 1];

/* Convert the RP2350 board unique ID into a hex ASCII string for the serial
 * descriptor. PICO_UNIQUE_BOARD_ID_SIZE_BYTES = 8 → 16 hex chars. */
static size_t get_serial_hex(char *out, size_t out_len)
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    static const char hex[] = "0123456789ABCDEF";
    size_t written = 0;
    for (size_t i = 0; i < sizeof(id.id) && written + 2 < out_len; i++) {
        out[written++] = hex[id.id[i] >> 4];
        out[written++] = hex[id.id[i] & 0xFu];
    }
    return written;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count = 0;

    if (index == STRID_LANGID) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == STRID_SERIAL) {
        char serial_buf[32];
        chr_count = get_serial_hex(serial_buf, sizeof(serial_buf));
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)serial_buf[i];
        }
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        if (str == NULL) return NULL;
        chr_count = strlen(str);
        const size_t max_count = (sizeof(_desc_str) / sizeof(_desc_str[0])) - 1;
        if (chr_count > max_count) chr_count = max_count;
        for (size_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = (uint16_t)str[i];
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
