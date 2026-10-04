/*
 * events.h - 本地归一化输入事件
 *
 * 平台适配层（SDL / Web 重放 / 测试）负责把各自的原始事件翻译成
 * InputEvent。核心逻辑只依赖本结构，因此：
 *   - Windows VK / Linux X11 keysym / macOS 键位差异被挡在核心之外；
 *   - 重放与真实输入走同一条处理路径（origin 区分信任级别）。
 */
#ifndef INPUT_STRATEGY_EVENTS_H
#define INPUT_STRATEGY_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#include "key.h"

#define EVENT_TEXT_MAX 64

/* 事件来源：决定安全闸（见 replay.h） */
typedef enum {
    ORIGIN_HUMAN = 0,    /* 现场真人在设备上的操作，完整权限 */
    ORIGIN_REPLAY = 1,   /* 本地日志重放，业务确认会被拒绝 */
    ORIGIN_REMOTE = 2,   /* 网页下发的实验输入，默认不启用，仅模拟器 */
} EventOrigin;

typedef enum {
    IE_KEY = 1,
    IE_TEXT,             /* IME 已提交文本 / 单字符输入 */
    IE_COMPOSITION,      /* IME 组合中（中文输入未确定） */
    IE_FOCUS_LOST,
    IE_FOCUS_GAINED,
    IE_DISPLAY_LOST,     /* 全屏切换中显示器被拔出等 */
    IE_DISPLAY_RESTORED,
} EventType;

typedef struct {
    EventType type;
    EventOrigin origin;
    NK_Key key;
    uint32_t mods;
    bool down;           /* true=按下, false=抬起 */
    bool repeat;         /* 操作系统自动重复事件 */
    bool composition;    /* 事件产生于 IME 组合态 */
    uint32_t raw_code;   /* 平台原始码（仅诊断/日志） */
    uint32_t platform;   /* 来源平台标识（见 PLATFORM_*） */
    char text[EVENT_TEXT_MAX]; /* IE_TEXT / IE_COMPOSITION 的内容 */
} InputEvent;

#define PLATFORM_SDL_LINUX   1
#define PLATFORM_SDL_WINDOWS 2
#define PLATFORM_SDL_MACOS   3
#define PLATFORM_WEB         4
#define PLATFORM_TEST        5
#define PLATFORM_REPLAY      6

const char *origin_name(EventOrigin o);

/* 构造辅助函数 */
InputEvent ie_key(EventOrigin origin, NK_Key key, uint32_t mods, bool down,
                  bool repeat, bool in_composition);

#endif
