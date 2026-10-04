#include "key.h"

#include <stdio.h>
#include <string.h>

static const struct { NK_Key key; const char *name; } NAMES[] = {
    {NK_ESC, "Esc"},
    {NK_F1, "F1"}, {NK_F2, "F2"}, {NK_F3, "F3"}, {NK_F4, "F4"},
    {NK_F5, "F5"}, {NK_F6, "F6"}, {NK_F7, "F7"}, {NK_F8, "F8"},
    {NK_F9, "F9"}, {NK_F10, "F10"}, {NK_F11, "F11"}, {NK_F12, "F12"},
    {NK_GRAVE, "Grave"},
    {NK_1, "1"}, {NK_2, "2"}, {NK_3, "3"}, {NK_4, "4"},
    {NK_5, "5"}, {NK_6, "6"}, {NK_7, "7"}, {NK_8, "8"},
    {NK_9, "9"}, {NK_0, "0"},
    {NK_MINUS, "Minus"}, {NK_EQUAL, "Equal"}, {NK_BACKSPACE, "Backspace"},
    {NK_TAB, "Tab"},
    {NK_Q, "Q"}, {NK_W, "W"}, {NK_E, "E"}, {NK_R, "R"}, {NK_T, "T"},
    {NK_Y, "Y"}, {NK_U, "U"}, {NK_I, "I"}, {NK_O, "O"}, {NK_P, "P"},
    {NK_LBRACKET, "BracketLeft"}, {NK_RBRACKET, "BracketRight"},
    {NK_BACKSLASH, "Backslash"},
    {NK_A, "A"}, {NK_S, "S"}, {NK_D, "D"}, {NK_F, "F"}, {NK_G, "G"},
    {NK_H, "H"}, {NK_J, "J"}, {NK_K, "K"}, {NK_L, "L"},
    {NK_SEMICOLON, "Semicolon"}, {NK_APOSTROPHE, "Quote"},
    {NK_RETURN, "Enter"},
    {NK_Z, "Z"}, {NK_X, "X"}, {NK_C, "C"}, {NK_V, "V"}, {NK_B, "B"},
    {NK_N, "N"}, {NK_M, "M"}, {NK_COMMA, "Comma"}, {NK_PERIOD, "Period"},
    {NK_SLASH, "Slash"},
    {NK_SPACE, "Space"},
    {NK_LEFT, "ArrowLeft"}, {NK_RIGHT, "ArrowRight"},
    {NK_UP, "ArrowUp"}, {NK_DOWN, "ArrowDown"},
    {NK_LSHIFT, "ShiftLeft"}, {NK_RSHIFT, "ShiftRight"},
    {NK_LCTRL, "ControlLeft"}, {NK_RCTRL, "ControlRight"},
    {NK_LALT, "AltLeft"}, {NK_RALT, "AltRight"},
    {NK_LGUI, "MetaLeft"}, {NK_RGUI, "MetaRight"},
    {NK_HOME, "Home"}, {NK_END, "End"},
    {NK_PAGEUP, "PageUp"}, {NK_PAGEDOWN, "PageDown"},
    {NK_INSERT, "Insert"}, {NK_DELETE, "Delete"},
    {NK_CAPSLOCK, "CapsLock"}, {NK_PRINTSCREEN, "PrintScreen"},
    {NK_SCROLLLOCK, "ScrollLock"}, {NK_PAUSE, "Pause"},
};

const char *nk_name(NK_Key key) {
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; ++i)
        if (NAMES[i].key == key) return NAMES[i].name;
    return "Unknown";
}

NK_Key nk_from_name(const char *name) {
    if (!name) return NK_UNKNOWN;
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; ++i)
        if (strcmp(NAMES[i].name, name) == 0) return NAMES[i].key;
    return NK_UNKNOWN;
}

bool nk_is_modifier(NK_Key key) {
    switch (key) {
    case NK_LSHIFT: case NK_RSHIFT:
    case NK_LCTRL: case NK_RCTRL:
    case NK_LALT: case NK_RALT:
    case NK_LGUI: case NK_RGUI:
        return true;
    default:
        return false;
    }
}

bool chord_equal(KeyChord a, KeyChord b) {
    return a.key == b.key && a.mods == b.mods;
}

const char *chord_to_str(KeyChord c) {
    static char buf[64];
    size_t n = 0;
    if (c.mods & KM_CTRL)  n += (size_t)snprintf(buf + n, sizeof buf - n, "Ctrl+");
    if (c.mods & KM_ALT)   n += (size_t)snprintf(buf + n, sizeof buf - n, "Alt+");
    if (c.mods & KM_SHIFT) n += (size_t)snprintf(buf + n, sizeof buf - n, "Shift+");
    if (c.mods & KM_GUI)   n += (size_t)snprintf(buf + n, sizeof buf - n, "Meta+");
    snprintf(buf + n, sizeof buf - n, "%s", nk_name(c.key));
    return buf;
}

bool chord_from_str(const char *s, KeyChord *out) {
    if (!s || !out) return false;
    uint32_t mods = 0;
    const char *p = s;
    for (;;) {
        if (strncmp(p, "Ctrl+", 5) == 0)       { mods |= KM_CTRL;  p += 5; }
        else if (strncmp(p, "Alt+", 4) == 0)   { mods |= KM_ALT;   p += 4; }
        else if (strncmp(p, "Shift+", 6) == 0) { mods |= KM_SHIFT; p += 6; }
        else if (strncmp(p, "Meta+", 5) == 0)  { mods |= KM_GUI;   p += 5; }
        else break;
    }
    NK_Key k = nk_from_name(p);
    if (k == NK_UNKNOWN) return false;
    out->key = k;
    out->mods = mods;
    return true;
}

const char *nk_diag(NK_Key key, uint32_t platform, uint32_t raw) {
    static char buf[96];
    snprintf(buf, sizeof buf, "%s[plat=%u raw=%u]", nk_name(key), platform, raw);
    return buf;
}
