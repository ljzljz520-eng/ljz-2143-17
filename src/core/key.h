/*
 * key.h - 归一化按键标识
 *
 * 跨平台策略：匹配一律使用"物理位置"标识 NK_*（对应 SDL scancode 语义），
 * 而不是平台相关的 keycode。这样在不同键盘布局、不同操作系统（
 * Windows 的 VK 码 / Linux evdev / macOS 键位 / Web KeyboardEvent.code）
 * 之间，策略表达保持一致。平台侧的原始码仅写入诊断日志，不参与匹配。
 */
#ifndef INPUT_STRATEGY_KEY_H
#define INPUT_STRATEGY_KEY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NK_UNKNOWN = 0,

    NK_ESC, NK_F1, NK_F2, NK_F3, NK_F4, NK_F5, NK_F6, NK_F7, NK_F8,
    NK_F9, NK_F10, NK_F11, NK_F12,

    NK_GRAVE, NK_1, NK_2, NK_3, NK_4, NK_5, NK_6, NK_7, NK_8, NK_9,
    NK_0, NK_MINUS, NK_EQUAL, NK_BACKSPACE,

    NK_TAB, NK_Q, NK_W, NK_E, NK_R, NK_T, NK_Y, NK_U, NK_I, NK_O, NK_P,
    NK_LBRACKET, NK_RBRACKET, NK_BACKSLASH,

    NK_A, NK_S, NK_D, NK_F, NK_G, NK_H, NK_J, NK_K, NK_L, NK_SEMICOLON,
    NK_APOSTROPHE, NK_RETURN,

    NK_Z, NK_X, NK_C, NK_V, NK_B, NK_N, NK_M, NK_COMMA, NK_PERIOD,
    NK_SLASH,

    NK_SPACE,
    NK_LEFT, NK_RIGHT, NK_UP, NK_DOWN,
    NK_LSHIFT, NK_RSHIFT, NK_LCTRL, NK_RCTRL, NK_LALT, NK_RALT,
    NK_LGUI, NK_RGUI,
    NK_HOME, NK_END, NK_PAGEUP, NK_PAGEDOWN, NK_INSERT, NK_DELETE,
    NK_CAPSLOCK, NK_PRINTSCREEN, NK_SCROLLLOCK, NK_PAUSE,

    /* 方向键位（小键盘布局）—— 物理位置归一化时也使用 NK_LEFT 等，
       这里保留枚举仅用于诊断。 */
    NK_KP_LEFT = NK_LEFT
} NK_Key;

/* 修饰键位掩码 */
#define KM_SHIFT   0x01u
#define KM_CTRL    0x02u
#define KM_ALT     0x04u
#define KM_GUI     0x08u  /* Windows 键 / macOS Command */

/* 一个物理键 + 修饰集合 = 一个"和弦"（chord） */
typedef struct {
    NK_Key key;
    uint32_t mods;
} KeyChord;

bool chord_equal(KeyChord a, KeyChord b);

/*
 * 文本名 <-> 枚举。网页、策略 JSON、日志全部使用稳定文本名，
 * 这样不同平台重放时互不依赖原始键值。
 * 例如 "F11"、"Esc"、"Left"、"Ctrl+Q"。
 */
const char *nk_name(NK_Key key);
NK_Key nk_from_name(const char *name);
const char *chord_to_str(KeyChord chord);   /* 返回线程安全静态缓冲 */
bool chord_from_str(const char *s, KeyChord *out);

bool nk_is_modifier(NK_Key key);

/* 平台适配用：诊断名（包含来源平台与原始码），不参与匹配 */
const char *nk_diag(NK_Key key, uint32_t platform, uint32_t raw);

#endif
