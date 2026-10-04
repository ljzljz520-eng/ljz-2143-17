/* font.h - 内置 5x7 点阵字体（ASCII；非 ASCII 字符以方块占位，
   实际中文通过 IME 提交，界面标签使用英文/拼音避免渲染依赖） */
#ifndef UI_FONT_H
#define UI_FONT_H
#include <stdbool.h>
#define FONT_W 5
#define FONT_H 7
const unsigned char *font_glyph(char c); /* 返回 7 字节，每字节低 5 位 */
#endif
