/*
 * sync.h - 设备侧策略同步
 *
 * 流程：GET /api/devices/{id}/policy 拉信封 -> policy_parse_envelope
 * 校验（schema + SHA-256 摘要）-> payload 再校验 ->
 *   通过：disp_stage_and_commit（激活边界），POST applied 回执
 *   失败：保留当前策略，POST rejected 回执（原因）
 * 网络不可达 / 非 200：静默保留上一良好策略（last-known-good）。
 */
#ifndef INPUT_STRATEGY_SYNC_H
#define INPUT_STRATEGY_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/dispatcher.h"

typedef struct {
    const char *base_url;   /* 如 http://127.0.0.1:8080 */
    const char *device_id;
} SyncConfig;

typedef struct {
    uint32_t polls;
    uint32_t applied;
    uint32_t rejected;
    uint32_t network_errors;
    uint64_t last_seen_generation;
} SyncStats;

void sync_init(SyncConfig cfg, SyncStats *stats);

/* 执行一次拉取/激活/回执。now_ms 用于激活边界。
   released_out 输出本次激活合成抬起的键数（可 NULL）。 */
bool sync_poll(Dispatcher *d, uint32_t now_ms, uint32_t *released_out);

const SyncStats *sync_stats(void);

#endif
