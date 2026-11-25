/* HID usage + modifiers -> ASCII, US layout (spec input §5.3). */
#include "input.h"

/* 0x1E..0x27 digits; 0x2D..0x38 symbols (0x32 is HID's non-US '#'). */
static const char digits[] = "1234567890", digits_s[] = "!@#$%^&*()";
static const char syms[] = "-=[]\\#;'`,./", syms_s[] = "_+{}|~:\"~<>?";

uint8_t input_key_char(uint8_t c, uint32_t mods) {
    bool sh = (mods & MOD_SHIFT) != 0;
    if (c >= 0x04 && c <= 0x1D) {
        if (mods & MOD_CTRL) return (uint8_t)(c - 3u);
        return (uint8_t)(c - 4u + (sh ? 'A' : 'a'));
    }
    if (c >= 0x1E && c <= 0x27) return (uint8_t)(sh ? digits_s : digits)[c - 0x1Eu];
    if (c >= 0x2D && c <= 0x38) return (uint8_t)(sh ? syms_s : syms)[c - 0x2Du];
    switch (c) {
    case 0x28: return 10;      /* Enter */
    case 0x29: return 27;      /* Esc */
    case 0x2A: return 8;       /* Backspace */
    case 0x2B: return 9;       /* Tab */
    case 0x2C: return 32;      /* Space */
    case 0x4C: return 127;     /* Delete */
    default:   return 0;
    }
}
