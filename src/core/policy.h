/*
 * policy.h - 输入策略（映射）：解析、校验、冲突检测、摘要
 *
 * 策略 JSON 分两层：
 *   envelope = {generation, device_id, issued_at, payload, digest}
 *   payload  = {generation, move_*, bindings:[{chord,command}]}
 *   digest   = sha256_hex(canonical_json(payload))
 *
 * 客户端与服务端执行同一套规则（C 实现在本文件，参考实现见
 * service/validator.py）。任何损坏包都会被拒绝并保留上一版策略。
 */
#ifndef INPUT_STRATEGY_POLICY_H
#define INPUT_STRATEGY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "command.h"
#include "json.h"
#include "key.h"

#define POLICY_MAX_BINDINGS 32
#define POLICY_ERR_LEN 256
#define POLICY_PAYLOAD_MAX 16384
#define POLICY_DEVICE_MAX 64
#define POLICY_ISSUED_AT_MAX 40

typedef enum {
    LAYER_PRESENTATION = 1,
    LAYER_MODAL = 2
} Layer;

typedef struct {
    KeyChord chord;
    char chord_str[24];
    CommandId command;
    char command_str[32];
    Layer layer;
} PolicyBinding;

typedef struct {
    uint64_t generation;
    uint32_t move_initial_delay_ms;
    uint32_t move_repeat_ms;
    uint32_t move_max_hold_ms;
    int32_t bounds_w;
    int32_t bounds_h;
    PolicyBinding bindings[POLICY_MAX_BINDINGS];
    uint32_t binding_count;
} Policy;

/* ---- 校验报告（结构化，服务接口直接消费） ---- */

#define VAL_MAX_ISSUES 24
#define VAL_CONFLICT_PAIRS 16

typedef enum {
    VAL_OK = 0,
    VAL_ERR_SCHEMA,        /* 结构/类型/范围错误 */
    VAL_ERR_RESERVED,      /* 使用了保留键 */
    VAL_ERR_DUP_CHORD,     /* 同一和弦重复映射 */
    VAL_ERR_DUP_COMMAND,   /* 同一命令被多个和弦绑定 */
    VAL_ERR_NOT_ALLOWED,   /* 命令不允许在该层使用 */
    VAL_ERR_MISSING,       /* 缺少必需绑定 */
    VAL_ERR_DIGEST,        /* 信封摘要不匹配/字段缺失 */
    VAL_ERR_INTERNAL
} ValCode;

typedef struct {
    ValCode code;
    char detail[96];
} ValIssue;

typedef struct {
    bool ok;
    ValIssue issues[VAL_MAX_ISSUES];
    uint32_t issue_count;
    /* 冲突明细（与 issues 对应，便于网页高亮两组输入框） */
    struct {
        char a[24]; char cmd_a[32];
        char b[24]; char cmd_b[32];
    } conflicts[VAL_CONFLICT_PAIRS];
    uint32_t conflict_count;
    /* 保留键明细 */
    char reserved[8][24];
    uint32_t reserved_count;
} ValidationReport;

void validation_init(ValidationReport *r);
void validation_add_issue(ValidationReport *r, ValCode code, const char *fmt, ...);

/* 判断某和弦是否为保留键（任何层、任何命令都不可映射）。
   返回非空时给出原因。 */
const char *policy_reserved_reason(KeyChord chord);
/* 模态层 Esc 始终是"关闭面板"，任何模态绑定到 Esc 都无效/被拒。 */
bool policy_modal_esc_is_fixed(void);

/* 默认（出厂）策略：设备首次启动、服务不可达、包损坏时使用。 */
void policy_default(Policy *out);

/* 对 payload JSON 文本做完整校验；通过则填充 out 并返回 true。
   report 可传 NULL。无论成败 report 都会写入问题明细。 */
bool policy_validate_payload_text(const char *payload_json, Policy *out,
                                  ValidationReport *report);

/* 对 payload 已解析树做校验（服务端模拟器复用）。 */
bool policy_validate_payload(const JsonVal *payload, Policy *out,
                             ValidationReport *report);

/* 信封解析与摘要校验。payload_json_out 输出 payload 的规范化文本
   （调用方 free），envelope 字段通过 out 参数返回。
   失败时 report->code 以 VAL_ERR_DIGEST/SCHEMA 表示。 */
typedef struct {
    uint64_t generation;
    char device_id[POLICY_DEVICE_MAX];
    char issued_at[POLICY_ISSUED_AT_MAX];
    char digest[65];
} PolicyEnvelope;

bool policy_parse_envelope(const char *envelope_json,
                           PolicyEnvelope *env_out,
                           char **payload_canonical_out,
                           ValidationReport *report);

/* 查映射：某层某和弦 -> 命令。未命中返回 CMD_NONE。 */
CommandId policy_lookup(const Policy *p, Layer layer, KeyChord chord);

/* 必需绑定（发布前必须存在）。 */
bool policy_has_required_bindings(const Policy *p);

/* 把 payload 树转成策略 JSON 文本（测试/默认策略生成用），调用方 free。 */
char *policy_default_payload_json(uint64_t generation);

#endif
