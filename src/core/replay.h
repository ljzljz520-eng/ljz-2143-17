/*
 * replay.h - 日志重放
 *
 * 安全语义（关键）：
 *   - 重放只重放"归一化输入帧"，命令由当前策略+核心重新计算，
 *     保证重放走的是与真人完全相同的代码路径；
 *   - 所有重放输入的 origin 强制为 ORIGIN_REPLAY。安全闸会拦截
 *     CMD_CONFIRM_BUSINESS —— 网页重放只能复现实验输入，
 *     绝不可能伪造现场人员已执行的业务确认；
 *   - 被拦截的确认写入 R_REPLAY_BLOCKED 审计帧与统计；
 *   - 日志帧 CRC 错误/截断立即中止重放（不会半可信地继续）；
 *   - 重放不产生任何真实全屏/退出副作用：使用 headless dispatcher，
 *     CMD_QUIT 仅记录、不退出宿主进程。
 */
#ifndef INPUT_STRATEGY_REPLAY_H
#define INPUT_STRATEGY_REPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "dispatcher.h"
#include "journal.h"
#include "policy.h"

typedef struct {
    uint32_t frames_input;
    uint32_t frames_system;
    uint32_t commands_emitted;
    uint32_t blocked_business;
    uint32_t duration_ms;
    bool corrupt;
    char corrupt_reason[128];
} ReplaySummary;

/* 以给定策略从头重放一段内存日志（服务端/网页同样可请求复算）。
   策略由调用方指定（用于"用新代次策略回放旧输入"的实验）。
   disp_out 可传 NULL；传入时调用方拥有该 dispatcher，可用
   replay_drain() 取出全部命令序列做断言。 */
bool replay_memory(const uint8_t *data, size_t len, const Policy *policy,
                   ReplaySummary *out, Dispatcher *disp_out);

/* 重放日志文件（C 客户端本地重放使用，headless） */
bool replay_file(const char *path, const Policy *policy, ReplaySummary *out,
                 Dispatcher *disp_out);

/* 重放结束后从队列取出全部命令（测试断言/网页展示命令序列）。 */
typedef struct { const char *name; int32_t arg; EventOrigin origin; } ReplayCmd;
uint32_t replay_drain(Dispatcher *d, ReplayCmd *out, uint32_t max);

#endif
