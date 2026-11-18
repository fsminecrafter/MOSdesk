#ifndef MOSDESK_INPUT_H
#define MOSDESK_INPUT_H

#include <stdint.h>

#include "syscall.h"

#define MOSDESK_INPUT_MOD_SHIFT (1u << 0u)
#define MOSDESK_INPUT_MOD_ALTGR (1u << 3u)   /* SYSCALL_INPUT_MOD_ALTGR */
#define MOSDESK_USB_MOD_LSHIFT (1u << 1u)
#define MOSDESK_USB_MOD_RALT (1u << 6u)

/* Input-event modifiers -> the HID modifier byte SYS_USB_KEY_TO_ASCII
 * takes: Shift, and AltGr for the layout's third characters ({ [ ] } \ @
 * on Nordic and German keyboards). */
static inline uint8_t mosdesk_usb_modifiers(uint8_t input_modifiers) {
    return (uint8_t)(((input_modifiers & MOSDESK_INPUT_MOD_SHIFT)
                ? MOSDESK_USB_MOD_LSHIFT : 0u) |
           ((input_modifiers & MOSDESK_INPUT_MOD_ALTGR)
                ? MOSDESK_USB_MOD_RALT : 0u));
}

#endif
