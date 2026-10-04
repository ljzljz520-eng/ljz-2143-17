/*
 * activation.h - 远程策略在"键仍按住"时到达的激活边界
 *
 * 规则（详见 docs/design.md "激活边界"）：
 *   T0 收到新信封并校验通过 -> 暂存（staged），不改变任何当前映射；
 *   T1 调用 activation_commit() 才原子切换。commit 时：
 *     1. 所有当前仍按住的物理键，按【旧策略】合成 KEYUP 并走完旧映射
 *        的收尾（停止连发、解除全屏 ARM），数量计入 released；
 *     2. 修饰键集合清零：带修饰的和弦必须全部重新按下才生效；
 *     3. 原子替换策略；
 *   T2 之后到达的事件一律用新策略解释。
 *
 * 不变式：切换之后没有任何键处于"按下"状态，从根本上消除卡键；
 * 物理键仍按着时操作系统的后续重复事件，因 keys_down 已不含该键，
 * 要么被当普通 keydown（新映射）处理，要么被 repeat 标志丢弃。
 */
#ifndef INPUT_STRATEGY_ACTIVATION_H
#define INPUT_STRATEGY_ACTIVATION_H

#include <stdbool.h>
#include <stdint.h>

#include "events.h"
#include "policy.h"

#define ACT_HELD_KEYS_MAX 16

typedef struct {
    bool staged;
    Policy staged_policy;
    PolicyEnvelope staged_env;
    Policy current;
    bool has_current;
    /* 提交瞬间需要合成抬起的键，由 dispatcher 读出并清空 */
    NK_Key release_list[ACT_HELD_KEYS_MAX];
    uint32_t release_count;
    uint32_t mods_to_clear;   /* 位掩码与 KM_* 对应，提示 dispatcher 清零 */
    /* 当前物理按住的键（边界计算依据） */
    NK_Key held[ACT_HELD_KEYS_MAX];
    bool held_down[ACT_HELD_KEYS_MAX];
} Activation;

void act_init(Activation *a);
void act_load_initial(Activation *a, const Policy *p);

/* 暂存一版已校验通过的策略（envelope 用于审计字段） */
void act_stage(Activation *a, const Policy *p, const PolicyEnvelope *env);

/* dispatcher 登记/注销物理键状态 */
void act_mark_down(Activation *a, NK_Key key, uint32_t mods);
void act_mark_up(Activation *a, NK_Key key);
bool act_is_down(Activation *a, NK_Key key);
uint32_t act_held_count(Activation *a);

/* 提交：生成旧映射下的 release_list。
   必须由 dispatcher 在停止连发/解除边沿 ARM 之后、替换策略之前完成，
   并在之后调用 act_commit_finish()。 */
uint32_t act_commit_prepare(Activation *a);
void act_commit_finish(Activation *a);

const Policy *act_current(const Activation *a);
uint64_t act_generation(const Activation *a);
bool act_has_staged(const Activation *a);

#endif
