
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

// TinyUSB common settings
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#define CFG_TUSB_DEBUG 0

// Enable USB Host mode
#define CFG_TUH_ENABLED 1

// Support one connected USB device
#define CFG_TUH_DEVICE_MAX 1
#define CFG_TUH_HUB 0

// Enable HID keyboard/scanner support
#define CFG_TUH_HID 1

// HID report buffer sizes
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 64

// USB descriptor buffer
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#ifdef __cplusplus
}
#endif

#endif
