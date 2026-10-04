#include "policy.h"

#include <stdarg.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

void validation_init(ValidationReport *r) {
    if (r) memset(r, 0, sizeof *r);
}

void validation_add_issue(ValidationReport *r, ValCode code, const char *fmt, ...) {
    if (!r || r->issue_count >= VAL_MAX_ISSUES) {
        if (r && code != VAL_OK) r->ok = false;
        return;
    }
    ValIssue *it = &r->issues[r->issue_count++];
    it->code = code;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it->detail, sizeof it->detail, fmt, ap);
    va_end(ap);
    r->ok = false;
}

const char *policy_reserved_reason(KeyChord chord) {
    /* 可靠退出通道在任何策略下都不得被改键：
       - Ctrl+Q / Meta+Q：跨平台应用退出（Linux/Win 常见 Ctrl+Q，macOS Cmd+Q）
       - Alt+F4：窗口管理器关闭（X11/Windows），macOS 由 Cmd+Q 覆盖
       窗口本身仍会收到 SDL_QUIT / WM_CLOSE，作为第二道退出通道。 */
    if (chord.key == NK_Q && (chord.mods & KM_CTRL) &&
        !(chord.mods & (KM_ALT | KM_SHIFT | KM_GUI)))
        return "Ctrl+Q is reserved for reliable quit";
    if (chord.key == NK_Q && (chord.mods & KM_GUI) &&
        !(chord.mods & (KM_ALT | KM_SHIFT | KM_CTRL)))
        return "Meta+Q / Cmd+Q is reserved for reliable quit";
    if (chord.key == NK_F4 && (chord.mods & KM_ALT) &&
        !(chord.mods & (KM_CTRL | KM_SHIFT | KM_GUI)))
        return "Alt+F4 is reserved for window manager close";
    return NULL;
}

bool policy_modal_esc_is_fixed(void) {
    /* 模态面板打开时按 Esc 必须始终能关闭面板；
       该绑定不接受网页改键，保证"可靠退出方式"。 */
    return true;
}

typedef struct {
    char a[24]; char cmd_a[32];
    char b[24]; char cmd_b[32];
} ConflictPair;

static void add_conflict(ValidationReport *r, const PolicyBinding *a,
                         const PolicyBinding *b) {
    if (r->conflict_count < VAL_CONFLICT_PAIRS) {
        ConflictPair *c = (ConflictPair *)&r->conflicts[r->conflict_count];
        snprintf(c->a, sizeof c->a, "%s", a->chord_str);
        snprintf(c->cmd_a, sizeof c->cmd_a, "%s", a->command_str);
        snprintf(c->b, sizeof c->b, "%s", b->chord_str);
        snprintf(c->cmd_b, sizeof c->cmd_b, "%s", b->command_str);
        r->conflict_count++;
    }
}

static bool command_allowed_in_payload(CommandId id) {
    /* 展示层白名单；禁止 quit / 文本 / 面板关闭 出现在可映射集合里。 */
    switch (id) {
    case CMD_OPEN_SETTINGS:
    case CMD_MOVE_LEFT:
    case CMD_MOVE_RIGHT:
    case CMD_MOVE_UP:
    case CMD_MOVE_DOWN:
    case CMD_TOGGLE_FULLSCREEN:
    case CMD_RESTORE_BACKGROUND:
    case CMD_CONFIRM_BUSINESS:
        return true;
    default:
        return false;
    }
}

bool policy_validate_payload(const JsonVal *pl, Policy *out,
                             ValidationReport *report) {
    validation_init(report);
    if (report) report->ok = true;
    if (!pl || pl->type != J_OBJ) {
        validation_add_issue(report, VAL_ERR_SCHEMA, "payload must be an object");
        return false;
    }

    Policy tmp;
    memset(&tmp, 0, sizeof tmp);
    bool ok = true;

    const JsonVal *jgen = json_obj_get(pl, "generation");
    int64_t gen;
    if (!json_int(jgen, &gen) || gen < 1) {
        validation_add_issue(report, VAL_ERR_SCHEMA,
                             "generation must be a positive integer");
        ok = false;
    }
    tmp.generation = (uint64_t)(gen > 0 ? gen : 0);

    struct { const char *k; uint32_t *dst; int32_t lo, hi; } u32f[] = {
        {"move_initial_delay_ms", &tmp.move_initial_delay_ms, 10, 1000},
        {"move_repeat_ms",        &tmp.move_repeat_ms,        10, 500},
        {"move_max_hold_ms",      &tmp.move_max_hold_ms,      100, 10000},
    };
    for (size_t i = 0; i < sizeof u32f / sizeof u32f[0]; ++i) {
        const JsonVal *jv = json_obj_get(pl, u32f[i].k);
        int64_t n;
        if (!json_int(jv, &n) || n < u32f[i].lo || n > u32f[i].hi) {
            validation_add_issue(report, VAL_ERR_SCHEMA,
                                 "%s must be integer in [%d,%d]",
                                 u32f[i].k, u32f[i].lo, u32f[i].hi);
            ok = false;
        } else {
            *u32f[i].dst = (uint32_t)n;
        }
    }
    int64_t bw, bh;
    const JsonVal *jbw = json_obj_get(pl, "bounds_w");
    const JsonVal *jbh = json_obj_get(pl, "bounds_h");
    if (!json_int(jbw, &bw) || bw < 100 || bw > 10000) {
        validation_add_issue(report, VAL_ERR_SCHEMA,
                             "bounds_w must be integer in [100,10000]");
        ok = false;
    } else tmp.bounds_w = (int32_t)bw;
    if (!json_int(jbh, &bh) || bh < 100 || bh > 10000) {
        validation_add_issue(report, VAL_ERR_SCHEMA,
                             "bounds_h must be integer in [100,10000]");
        ok = false;
    } else tmp.bounds_h = (int32_t)bh;
    if (ok && tmp.move_max_hold_ms < tmp.move_initial_delay_ms) {
        validation_add_issue(report, VAL_ERR_SCHEMA,
                             "move_max_hold_ms must be >= move_initial_delay_ms");
        ok = false;
    }

    const JsonVal *jbs = json_obj_get(pl, "bindings");
    if (!jbs || jbs->type != J_ARR) {
        validation_add_issue(report, VAL_ERR_SCHEMA, "bindings must be an array");
        return false;
    }

    for (uint32_t i = 0; i < jbs->u.arr.len; ++i) {
        const JsonVal *b = jbs->u.arr.items[i];
        char where[40];
        snprintf(where, sizeof where, "bindings[%u]", i);
        if (!b || b->type != J_OBJ) {
            validation_add_issue(report, VAL_ERR_SCHEMA,
                                 "%s must be an object", where);
            ok = false; continue;
        }
        /* 严格键集合，拒绝未知字段（拼写错误的键不得被静默忽略） */
        for (uint32_t k = 0; k < b->u.obj.len; ++k) {
            const char *key = b->u.obj.m[k].key;
            if (strcmp(key, "chord") && strcmp(key, "command")) {
                validation_add_issue(report, VAL_ERR_SCHEMA,
                                     "%s has unknown field '%s'", where, key);
                ok = false;
            }
        }
        const char *cs = json_str(json_obj_get(b, "chord"));
        const char *cm = json_str(json_obj_get(b, "command"));
        if (!cs || !cm) {
            validation_add_issue(report, VAL_ERR_SCHEMA,
                                 "%s requires string chord and command", where);
            ok = false; continue;
        }
        KeyChord chord;
        if (!chord_from_str(cs, &chord)) {
            validation_add_issue(report, VAL_ERR_SCHEMA,
                                 "%s: unknown chord '%s'", where, cs);
            ok = false; continue;
        }
        CommandId cmd = cmd_from_name(cm);
        if (cmd == CMD_NONE) {
            validation_add_issue(report, VAL_ERR_SCHEMA,
                                 "%s: unknown command '%s'", where, cm);
            ok = false; continue;
        }
        if (nk_is_modifier(chord.key)) {
            validation_add_issue(report, VAL_ERR_SCHEMA,
                                 "%s: cannot bind a modifier key alone", where);
            ok = false; continue;
        }
        const char *why = policy_reserved_reason(chord);
        if (why) {
            validation_add_issue(report, VAL_ERR_RESERVED,
                                 "%s: %s", where, why);
            if (report->reserved_count < 8) {
                snprintf(report->reserved[report->reserved_count], 24, "%s", cs);
                report->reserved_count++;
            }
            ok = false; continue;
        }
        if (!command_allowed_in_payload(cmd)) {
            validation_add_issue(report, VAL_ERR_NOT_ALLOWED,
                                 "%s: command '%s' is not remappable",
                                 where, cm);
            ok = false; continue;
        }
        if (tmp.binding_count >= POLICY_MAX_BINDINGS) {
            validation_add_issue(report, VAL_ERR_SCHEMA,
                                 "too many bindings (max %d)",
                                 POLICY_MAX_BINDINGS);
            ok = false; break;
        }
        PolicyBinding *pb = &tmp.bindings[tmp.binding_count++];
        pb->chord = chord;
        pb->command = cmd;
        pb->layer = LAYER_PRESENTATION;
        snprintf(pb->chord_str, sizeof pb->chord_str, "%s", cs);
        snprintf(pb->command_str, sizeof pb->command_str, "%s", cm);
    }

    /* 冲突检测：双向
       1) 同一和弦 -> 两个命令：按键语义不确定（网页高亮两处）
       2) 同一命令 <- 两个和弦：无法预测哪次按下触发，同样拒绝
       （"二义映射" 正是冲突的定义；空键位是允许的，只是必需性检查会报错） */
    for (uint32_t i = 0; i < tmp.binding_count; ++i) {
        for (uint32_t j = i + 1; j < tmp.binding_count; ++j) {
            PolicyBinding *a = &tmp.bindings[i], *b = &tmp.bindings[j];
            if (chord_equal(a->chord, b->chord)) {
                validation_add_issue(report, VAL_ERR_DUP_CHORD,
                     "chord '%s' maps to both '%s' and '%s'",
                     a->chord_str, a->command_str, b->command_str);
                add_conflict(report, a, b);
                ok = false;
            }
            if (a->command == b->command) {
                validation_add_issue(report, VAL_ERR_DUP_COMMAND,
                     "command '%s' is bound to both '%s' and '%s'",
                     a->command_str, a->chord_str, b->chord_str);
                add_conflict(report, a, b);
                ok = false;
            }
        }
    }

    /* 必需绑定：现场操作的基本语义必须齐全 */
    static const CommandId required[] = {
        CMD_OPEN_SETTINGS, CMD_TOGGLE_FULLSCREEN, CMD_RESTORE_BACKGROUND,
        CMD_MOVE_LEFT, CMD_MOVE_RIGHT, CMD_MOVE_UP, CMD_MOVE_DOWN,
        CMD_CONFIRM_BUSINESS
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < tmp.binding_count; ++j)
            if (tmp.bindings[j].command == required[i]) found = true;
        if (!found) {
            validation_add_issue(report, VAL_ERR_MISSING,
                                 "missing required binding for '%s'",
                                 cmd_name(required[i]));
            ok = false;
        }
    }

    if (ok && out) *out = tmp;
    return ok;
}

bool policy_validate_payload_text(const char *payload_json, Policy *out,
                                  ValidationReport *report) {
    char err[POLICY_ERR_LEN];
    JsonVal *v = json_parse(payload_json, err, sizeof err);
    if (!v) {
        validation_init(report);
        validation_add_issue(report, VAL_ERR_SCHEMA, "%s", err);
        return false;
    }
    bool r = policy_validate_payload(v, out, report);
    json_free(v);
    return r;
}

CommandId policy_lookup(const Policy *p, Layer layer, KeyChord chord) {
    if (!p) return CMD_NONE;
    for (uint32_t i = 0; i < p->binding_count; ++i) {
        const PolicyBinding *b = &p->bindings[i];
        if (b->layer == layer && chord_equal(b->chord, chord))
            return b->command;
    }
    return CMD_NONE;
}

bool policy_has_required_bindings(const Policy *p) {
    /* 轻量判定：直接检查命令覆盖（避免重新解析） */
    static const CommandId required[] = {
        CMD_OPEN_SETTINGS, CMD_TOGGLE_FULLSCREEN, CMD_RESTORE_BACKGROUND,
        CMD_MOVE_LEFT, CMD_MOVE_RIGHT, CMD_MOVE_UP, CMD_MOVE_DOWN,
        CMD_CONFIRM_BUSINESS
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < p->binding_count; ++j)
            if (p->bindings[j].command == required[i]) found = true;
        if (!found) return false;
    }
    return true;
}

void policy_default(Policy *out) {
    ValidationReport r;
    char *j = policy_default_payload_json(1);
    bool ok = policy_validate_payload_text(j, out, &r);
    free(j);
    /* 内置默认策略自身必须通过校验，否则属于编译期缺陷 */
    if (!ok) {
        fprintf(stderr, "FATAL: built-in default policy is invalid\n");
        abort();
    }
}

char *policy_default_payload_json(uint64_t generation) {
    static const struct { const char *chord; const char *cmd; } def[] = {
        {"Esc",              "open_settings"},
        {"F11",              "toggle_fullscreen"},
        {"Ctrl+R",           "restore_background"},
        {"ArrowLeft",        "move_left"},
        {"ArrowRight",       "move_right"},
        {"ArrowUp",          "move_up"},
        {"ArrowDown",        "move_down"},
        {"Enter",            "confirm_business"},
    };
    size_t cap = 1024, len = 0;
    char *buf = malloc(cap);
    len += (size_t)snprintf(buf + len, cap - len,
        "{\"bindings\":[");
    for (size_t i = 0; i < sizeof def / sizeof def[0]; ++i) {
        len += (size_t)snprintf(buf + len, cap - len,
            "%s{\"chord\":\"%s\",\"command\":\"%s\"}",
            i ? "," : "", def[i].chord, def[i].cmd);
        if (len + 256 > cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    /* 键按字典序输出（canonical），与 json_canonical / Python
       json.dumps(sort_keys=True, separators=(",",":")) 一致，
       使默认 payload 可直接用于摘要计算。 */
    snprintf(buf + len, cap - len,
        "],\"bounds_h\":720,\"bounds_w\":1280,\"generation\":%llu,"
        "\"move_initial_delay_ms\":350,\"move_max_hold_ms\":3000,"
        "\"move_repeat_ms\":60}",
        (unsigned long long)generation);
    return buf;
}

bool policy_parse_envelope(const char *envelope_json, PolicyEnvelope *env_out,
                           char **payload_canonical_out,
                           ValidationReport *report) {
    validation_init(report);
    if (report) report->ok = true;
    char err[POLICY_ERR_LEN];
    JsonVal *env = json_parse(envelope_json, err, sizeof err);
    if (!env || env->type != J_OBJ) {
        validation_add_issue(report, VAL_ERR_SCHEMA,
                             "envelope is not a JSON object: %s",
                             env ? "top-level is not object" : err);
        json_free(env);
        return false;
    }

    bool ok = true;
    PolicyEnvelope e;
    memset(&e, 0, sizeof e);

    const JsonVal *jgen  = json_obj_get(env, "generation");
    const JsonVal *jdev  = json_obj_get(env, "device_id");
    const JsonVal *jiss  = json_obj_get(env, "issued_at");
    const JsonVal *jpay  = json_obj_get(env, "payload");
    const JsonVal *jdig  = json_obj_get(env, "digest");
    int64_t gen;
    if (!json_int(jgen, &gen) || gen < 1) {
        validation_add_issue(report, VAL_ERR_SCHEMA,
                             "envelope.generation must be positive integer");
        ok = false;
    } else e.generation = (uint64_t)gen;
    const char *dev = json_str(jdev), *iss = json_str(jiss), *dig = json_str(jdig);
    if (!dev || !*dev || strlen(dev) >= POLICY_DEVICE_MAX) {
        validation_add_issue(report, VAL_ERR_SCHEMA, "bad envelope.device_id");
        ok = false;
    } else snprintf(e.device_id, sizeof e.device_id, "%s", dev);
    if (!iss || !*iss || strlen(iss) >= POLICY_ISSUED_AT_MAX) {
        validation_add_issue(report, VAL_ERR_SCHEMA, "bad envelope.issued_at");
        ok = false;
    } else snprintf(e.issued_at, sizeof e.issued_at, "%s", iss);
    if (!dig || strlen(dig) != 64) {
        validation_add_issue(report, VAL_ERR_SCHEMA,
                             "envelope.digest must be 64 hex chars");
        ok = false;
    } else snprintf(e.digest, sizeof e.digest, "%s", dig);
    if (!jpay || jpay->type != J_OBJ) {
        validation_add_issue(report, VAL_ERR_SCHEMA,
                             "envelope.payload must be an object");
        json_free(env);
        return false;
    }

    /* 信封严格键集合 */
    for (uint32_t k = 0; k < env->u.obj.len; ++k) {
        const char *key = env->u.obj.m[k].key;
        if (strcmp(key, "generation") && strcmp(key, "device_id") &&
            strcmp(key, "issued_at") && strcmp(key, "payload") &&
            strcmp(key, "digest")) {
            validation_add_issue(report, VAL_ERR_SCHEMA,
                                 "envelope has unknown field '%s'", key);
            ok = false;
        }
    }

    JsonBuf canon = {0};
    if (!json_canonical(jpay, &canon)) {
        validation_add_issue(report, VAL_ERR_INTERNAL, "canonicalization failed");
        json_free(env);
        return false;
    }
    char calc[65];
    sha256_hex(canon.data, canon.len, calc);
    if (ok && (!dig || strcasecmp(calc, dig) != 0)) {
        validation_add_issue(report, VAL_ERR_DIGEST,
                             "payload digest mismatch: payload was tampered or truncated");
        ok = false;
    }

    /* 代次内外一致性 */
    int64_t pgen;
    if (!json_int(json_obj_get(jpay, "generation"), &pgen) ||
        (ok && (uint64_t)pgen != e.generation)) {
        validation_add_issue(report, VAL_ERR_SCHEMA,
             "envelope.generation must equal payload.generation");
        ok = false;
    }

    if (ok) {
        if (env_out) *env_out = e;
        if (payload_canonical_out) *payload_canonical_out = canon.data;
        else free(canon.data);
    } else {
        free(canon.data);
    }
    json_free(env);
    return ok;
}
