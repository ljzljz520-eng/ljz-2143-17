/*
 * test_core.c - 核心输入策略系统的纯 C 单元测试（不依赖 SDL）。
 * 运行：make test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "activation.h"
#include "command.h"
#include "crc32.h"
#include "dispatcher.h"
#include "events.h"
#include "fullscreen.h"
#include "journal.h"
#include "json.h"
#include "key.h"
#include "policy.h"
#include "repeat.h"
#include "replay.h"
#include "sha256.h"

static int g_failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); } \
} while (0)
#define SECTION(name) printf("== %s ==\n", name)

/* ---------- 1. 归一化按键 / 和弦 ---------- */
static void test_keys(void) {
    SECTION("key normalization");
    CHECK(nk_from_name("F11") == NK_F11, "F11 name");
    CHECK(nk_from_name("ArrowLeft") == NK_LEFT, "ArrowLeft");
    KeyChord a, b;
    CHECK(chord_from_str("Ctrl+Q", &a), "parse Ctrl+Q");
    CHECK(a.key == NK_Q && a.mods == KM_CTRL, "Ctrl+Q values");
    CHECK(chord_from_str("Meta+Q", &b) && b.mods == KM_GUI, "Meta+Q");
    CHECK(!chord_from_str("Bogus+X", &b), "unknown key rejected");
    CHECK(!chord_from_str("Ctrl+Bogus", &b), "unknown mod target");
    CHECK(strcmp(chord_to_str((KeyChord){NK_R, KM_CTRL}), "Ctrl+R") == 0,
          "chord to str");
}

/* ---------- 2. JSON 严格解析 / 规范化 ---------- */
static void test_json(void) {
    SECTION("strict json + canonical");
    char err[128]; JsonBuf b = {0};
    CHECK(json_parse("{\"a\":1}2", err, sizeof err) == NULL, "trailing garbage");
    CHECK(json_parse("{\"a\":1,\"a\":2}", err, sizeof err) == NULL, "dup keys");
    CHECK(json_parse("{\"a\":1e3}", err, sizeof err) == NULL, "no exponent");
    CHECK(json_parse("{\"a\":\n}", err, sizeof err) == NULL, "control char");
    CHECK(json_canonical_text("{\"b\":2,\"a\":[3,2]}", &b, err, sizeof err),
          "canonical ok");
    CHECK(strcmp(b.data, "{\"a\":[3,2],\"b\":2}") == 0,
          "canonical sorted: %s", b.data);
    json_buf_free(&b);
    /* 与 Python json.dumps(sort_keys=True, separators) 的转义规则一致 */
    CHECK(json_canonical_text("{\"z\":\"a/b\\n\\u00e4\"}", &b, err, sizeof err),
          "escapes");
    CHECK(strcmp(b.data, "{\"z\":\"a/b\\n\xc3\xa4\"}") == 0,
          "slash not escaped, utf8 raw: %s", b.data);
    json_buf_free(&b);
}

/* ---------- 3. SHA256 / CRC32 基本正确性 ---------- */
static void test_hashes(void) {
    SECTION("hashes");
    char hex[65];
    sha256_hex("abc", 3, hex);
    CHECK(strcmp(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
        "sha256 abc");
    CHECK(crc32_ieee("123456789", 9) == 0xCBF43926, "crc32 check");
}

/* ---------- 4. 默认策略与服务端校验：保留键/冲突/缺失 ---------- */
static void test_policy(void) {
    SECTION("policy validation");
    Policy def;
    policy_default(&def);
    ValidationReport r;
    CHECK(policy_validate_payload_text(
              "{\"generation\":1,\"bindings\":[],"
              "\"move_initial_delay_ms\":350,\"move_repeat_ms\":60,"
              "\"move_max_hold_ms\":3000,\"bounds_w\":1280,\"bounds_h\":720}",
              NULL, &r) == false, "missing bindings rejected");
    CHECK(r.issue_count > 0, "issues recorded");

    /* 保留键 */
    char *pl = policy_default_payload_json(1);
    CHECK(policy_validate_payload_text(pl, &def, &r), "default valid");
    free(pl);
    static const char *reserved_chords[] = {"Ctrl+Q", "Meta+Q", "Alt+F4"};
    for (size_t i = 0; i < 3; ++i) {
        char buf[1024];
        snprintf(buf, sizeof buf,
            "{\"generation\":2,\"move_initial_delay_ms\":350,"
            "\"move_repeat_ms\":60,\"move_max_hold_ms\":3000,"
            "\"bounds_w\":1280,\"bounds_h\":720,\"bindings\":["
            "{\"chord\":\"Esc\",\"command\":\"open_settings\"},"
            "{\"chord\":\"F11\",\"command\":\"toggle_fullscreen\"},"
            "{\"chord\":\"Ctrl+R\",\"command\":\"restore_background\"},"
            "{\"chord\":\"ArrowLeft\",\"command\":\"move_left\"},"
            "{\"chord\":\"ArrowRight\",\"command\":\"move_right\"},"
            "{\"chord\":\"ArrowUp\",\"command\":\"move_up\"},"
            "{\"chord\":\"ArrowDown\",\"command\":\"move_down\"},"
            "{\"chord\":\"Enter\",\"command\":\"confirm_business\"},"
            "{\"chord\":\"%s\",\"command\":\"move_down\"}]}",
            reserved_chords[i]);
        Policy p; ValidationReport rr;
        bool ok = policy_validate_payload_text(buf, &p, &rr);
        CHECK(!ok, "reserved %s must be rejected", reserved_chords[i]);
        bool saw = false;
        for (uint32_t k = 0; k < rr.issue_count; ++k)
            if (rr.issues[k].code == VAL_ERR_RESERVED) saw = true;
        CHECK(saw, "reserved issue code for %s", reserved_chords[i]);
    }

    /* 冲突：同和弦两命令 / 同命令两和弦 */
    const char *dup_chord =
        "{\"generation\":3,\"move_initial_delay_ms\":350,\"move_repeat_ms\":60,"
        "\"move_max_hold_ms\":3000,\"bounds_w\":1280,\"bounds_h\":720,"
        "\"bindings\":["
        "{\"chord\":\"F11\",\"command\":\"toggle_fullscreen\"},"
        "{\"chord\":\"F11\",\"command\":\"restore_background\"}]}";
    Policy p; ValidationReport rr;
    CHECK(!policy_validate_payload_text(dup_chord, &p, &rr), "dup chord");
    bool saw = false;
    for (uint32_t k = 0; k < rr.issue_count; ++k)
        if (rr.issues[k].code == VAL_ERR_DUP_CHORD) saw = true;
    CHECK(saw, "dup chord code");

    const char *dup_cmd =
        "{\"generation\":4,\"move_initial_delay_ms\":350,\"move_repeat_ms\":60,"
        "\"move_max_hold_ms\":3000,\"bounds_w\":1280,\"bounds_h\":720,"
        "\"bindings\":["
        "{\"chord\":\"F11\",\"command\":\"toggle_fullscreen\"},"
        "{\"chord\":\"F12\",\"command\":\"toggle_fullscreen\"}]}";
    CHECK(!policy_validate_payload_text(dup_cmd, &p, &rr), "dup command");

    /* 参数越界 */
    char *bad = policy_default_payload_json(5);
    char *x = strstr(bad, "\"move_repeat_ms\":60");
    strcpy(x, "\"move_repeat_ms\":99999");
    CHECK(!policy_validate_payload_text(bad, &p, &rr), "range rejected");
    free(bad);
}

/* ---------- 5. 信封摘要：损坏包 ---------- */
static void test_envelope(void) {
    SECTION("envelope digest / corruption");
    char *payload = policy_default_payload_json(7);
    char digest[65];
    sha256_hex(payload, strlen(payload), digest);
    /* 构造信封 JSON（payload 内联）。用 Python 风格手写。 */
    char env[POLICY_PAYLOAD_MAX + 512];
    snprintf(env, sizeof env,
        "{\"generation\":7,\"device_id\":\"dev-001\","
        "\"issued_at\":\"2026-10-04T10:00:00Z\","
        "\"payload\":%s,\"digest\":\"%s\"}", payload, digest);
    PolicyEnvelope pe; char *canon = NULL; ValidationReport r;
    CHECK(policy_parse_envelope(env, &pe, &canon, &r), "valid envelope");
    CHECK(pe.generation == 7, "gen");
    free(canon);

    /* 篡改 payload 一个键值：F11 -> F10 */
    char *at = strstr(env, "\"F11\"");
    at[3] = '0';
    CHECK(!policy_parse_envelope(env, &pe, &canon, &r),
          "tampered envelope rejected");
    bool digest_issue = false;
    for (uint32_t i = 0; i < r.issue_count; ++i)
        if (r.issues[i].code == VAL_ERR_DIGEST) digest_issue = true;
    CHECK(digest_issue, "digest issue reported");

    /* 截断的 JSON */
    char trunc[256];
    size_t n = strlen(env);
    size_t cut = n > 200 ? 200 : n;
    memcpy(trunc, env, cut); trunc[cut] = 0;
    CHECK(!policy_parse_envelope(trunc, &pe, &canon, &r), "truncated rejected");
    free(payload);
}

/* ---------- 6. 分层 + IME 组合态 ---------- */
static void make_disp(Dispatcher *d, const Policy *p) {
    disp_init_headless(d, p, 0);
}
static void key(Dispatcher *d, EventOrigin o, NK_Key k, uint32_t m,
                bool down, bool repeat, bool comp) {
    InputEvent e = ie_key(o, k, m, down, repeat, comp);
    disp_handle(d, &e);
}
static const char *first_cmd(Dispatcher *d) {
    Command c;
    return cq_pop(&d->q, &c) ? cmd_name(c.id) : NULL;
}

static void test_layers_and_ime(void) {
    SECTION("layers + IME composition gate");
    Policy p; policy_default(&p);
    Dispatcher d; make_disp(&d, &p);

    /* 展示层：Esc -> open_settings */
    key(&d, ORIGIN_HUMAN, NK_ESC, 0, true, false, false);
    CHECK(strcmp(first_cmd(&d), "open_settings") == 0, "Esc opens panel");
    CHECK(disp_panel_open(&d), "panel open");

    /* 面板打开时按 Esc：关闭面板（固定，不查策略） */
    key(&d, ORIGIN_HUMAN, NK_ESC, 0, true, false, false);
    CHECK(strcmp(first_cmd(&d), "close_panel") == 0, "Esc closes panel");
    CHECK(!disp_panel_open(&d), "panel closed");

    /* 重新打开，F11 必须被模态吞掉 */
    key(&d, ORIGIN_HUMAN, NK_ESC, 0, true, false, false);
    first_cmd(&d);
    key(&d, ORIGIN_HUMAN, NK_F11, 0, true, false, false);
    CHECK(cq_count(&d.q) == 0, "F11 must not pass through modal");

    /* IME 组合态：中文输入中的 Esc / 方向键 / F11 不得触发快捷键 */
    InputEvent ce = {0};
    ce.type = IE_COMPOSITION; ce.origin = ORIGIN_HUMAN;
    snprintf(ce.text, sizeof ce.text, "nihao");
    disp_handle(&d, &ce);
    CHECK(d.panel.composing, "composition active");
    key(&d, ORIGIN_HUMAN, NK_F11, 0, true, false, true);
    key(&d, ORIGIN_HUMAN, NK_ESC, 0, true, false, true);
    key(&d, ORIGIN_HUMAN, NK_LEFT, 0, true, false, true);
    CHECK(cq_count(&d.q) == 0, "no hotkeys during composition");
    CHECK(d.stat_ime_swallowed >= 3, "ime swallow count %u", d.stat_ime_swallowed);
    /* 但 Ctrl+Q 在组合态仍必须可靠退出 */
    key(&d, ORIGIN_HUMAN, NK_Q, KM_CTRL, true, false, true);
    const char *qc = first_cmd(&d);
    CHECK(qc && strcmp(qc, "quit") == 0, "Ctrl+Q works in composition");

    /* 组合结束后 Esc 恢复关面板 */
    ce.text[0] = 0;
    disp_handle(&d, &ce);
    CHECK(!d.panel.composing, "composition end");

    /* TEXT 提交写入字段 */
    InputEvent te = {0};
    te.type = IE_TEXT; te.origin = ORIGIN_HUMAN;
    snprintf(te.text, sizeof te.text, "\xc4\xe3"); /* latin1 bytes, just payload */
    disp_handle(&d, &te);
    CHECK(d.panel.text[0] != 0, "text inserted");
}

/* ---------- 7. 边沿触发：全屏不被重复事件来回切换 ---------- */
static void test_fullscreen_edge(void) {
    SECTION("fullscreen edge trigger FSM");
    FullscreenFsm f; fs_init(&f, 500);
    /* 首次按下：触发 */
    CHECK(fs_request(&f, NK_F11, true, false, 0), "first F11 triggers");
    fs_switch_ok(&f, true);
    CHECK(f.state == FS_FULLSCREEN, "now fullscreen");
    /* 同键按住重复：忽略 */
    CHECK(!fs_request(&f, NK_F11, true, true, 50), "repeat swallowed");
    CHECK(f.state == FS_FULLSCREEN, "still fullscreen after repeats");
    /* 未抬起又一次 down(非repeat，某些平台合成)：也忽略 */
    CHECK(!fs_request(&f, NK_F11, true, false, 60), "second down swallowed");
    /* 抬起：不切换 */
    CHECK(!fs_request(&f, NK_F11, false, false, 70), "keyup no toggle");
    CHECK(f.armed_key == 0, "disarmed");
    /* 再按：退出全屏，仅此一次 */
    CHECK(fs_request(&f, NK_F11, true, false, 100), "second press exits");
    fs_switch_ok(&f, false);
    for (int i = 0; i < 5; ++i)
        CHECK(!fs_request(&f, NK_F11, true, true, 110 + i), "repeat ignored");
    CHECK(f.state == FS_WINDOWED, "windowed, not toggled back");
    fs_keyup(&f, NK_F11); /* 物理抬起（重复事件不会解除 ARM） */

    /* 切换中拔屏：不排队第二个切换；恢复后 reconcile 到实际模式 */
    CHECK(fs_request(&f, NK_F11, true, false, 1000), "request enter again");
    fs_display_lost(&f);
    CHECK(f.state == FS_RECONCILING, "reconciling after display lost");
    CHECK(!fs_request(&f, NK_F11, true, false, 1010),
          "no second toggle while reconciling");
    fs_reconcile_to(&f, false);
    CHECK(f.state == FS_WINDOWED, "settle windowed after recovery");

    /* 切换超时 -> reconcile 请求 */
    FullscreenFsm g; fs_init(&g, 200);
    CHECK(fs_request(&g, NK_F11, true, false, 0), "g request");
    CHECK(!fs_tick(&g, 100), "no timeout yet");
    bool need = fs_tick(&g, 201);
    CHECK(need, "timeout asks reconcile");
}

/* ---------- 8. 长按方向键：有界连发 + 边界钳制 ---------- */
static int g_clamp_after = -1;
static int g_moves = 0;
static bool fake_move(void *ud, CommandId cmd, int32_t steps) {
    (void)ud; (void)cmd; (void)steps;
    g_moves++;
    if (g_clamp_after >= 0 && g_moves > g_clamp_after) return false;
    return true;
}

static void test_repeat_bounded(void) {
    SECTION("bounded movement repeat");
    Policy p; policy_default(&p);
    Dispatcher d;
    DispatcherOps ops = {0};
    ops.do_move = fake_move;
    disp_init(&d, &p, ops, 0);
    g_moves = 0; g_clamp_after = 100000;

    key(&d, ORIGIN_HUMAN, NK_RIGHT, 0, true, false, false);
    int initial = g_moves; /* 立即 1 步 */
    CHECK(initial == 1, "immediate one step");
    /* 初始延迟内重复 tick 不再产生 */
    bool nr = false;
    disp_tick(&d, 300, &nr);
    CHECK(g_moves == 1, "no move during initial delay");
    disp_tick(&d, 350, &nr);
    CHECK(g_moves == 2, "first repeat after delay");
    for (uint32_t t = 410; t <= 600; t += 60)
        disp_tick(&d, t, &nr);
    CHECK(g_moves > 2, "repeats flow, got %d", g_moves);
    /* 越过最大保持时间后必须自动停止 */
    for (uint32_t t = 3100; t <= 6000; t += 60)
        disp_tick(&d, t, &nr);
    int at4 = g_moves;
    disp_tick(&d, 6000, &nr);
    disp_tick(&d, 7000, &nr);
    CHECK(g_moves == at4, "auto-stop at max_hold, moves=%d", g_moves);
    /* 抬起后无残留 */
    key(&d, ORIGIN_HUMAN, NK_RIGHT, 0, false, false, false);
    disp_tick(&d, 9000, &nr);
    CHECK(g_moves == at4, "nothing after keyup");

    /* 空间上界：emit false 立即停 */
    Dispatcher d2;
    g_moves = 0; g_clamp_after = 2;
    disp_init(&d2, &p, ops, 0);
    key(&d2, ORIGIN_HUMAN, NK_LEFT, 0, true, false, false);
    for (uint32_t t = 60; t <= 5000; t += 30)
        disp_tick(&d2, t, &nr);
    CHECK(g_moves <= 3, "clamped by viewport, moves=%d", g_moves);
    key(&d2, ORIGIN_HUMAN, NK_LEFT, 0, false, false, false);
}

/* ---------- 9. 失焦漏收抬键：无卡键 ---------- */
static void test_focus_lost(void) {
    SECTION("focus lost releases stuck keys");
    Policy p; policy_default(&p);
    Dispatcher d;
    DispatcherOps ops = {0};
    ops.do_move = fake_move;
    g_moves = 0; g_clamp_after = 100000;
    disp_init(&d, &p, ops, 0);
    key(&d, ORIGIN_HUMAN, NK_DOWN, 0, true, false, false);
    CHECK(act_held_count(&d.act) == 1, "held one");
    InputEvent fl = {0}; fl.type = IE_FOCUS_LOST; fl.origin = ORIGIN_HUMAN;
    disp_handle(&d, &fl);
    CHECK(act_held_count(&d.act) == 0, "all released on focus lost");
    bool nr = false;
    int before = g_moves;
    for (uint32_t t = 400; t < 5000; t += 50) disp_tick(&d, t, &nr);
    CHECK(g_moves == before, "no movement after focus lost");
    /* 重新聚焦，修饰状态从干净开始 */
    InputEvent fg = {0}; fg.type = IE_FOCUS_GAINED; fg.origin = ORIGIN_HUMAN;
    disp_handle(&d, &fg);
    CHECK(d.mods == 0, "mods cleared");
}

/* ---------- 10. 激活边界：按住中收到新策略 ---------- */
static void test_activation_boundary(void) {
    SECTION("activation boundary while keys held");
    Policy p; policy_default(&p);
    Dispatcher d; disp_init_headless(&d, &p, 0);

    /* 物理按住右方向键（移动连发中） */
    key(&d, ORIGIN_HUMAN, NK_RIGHT, 0, true, false, false);
    first_cmd(&d); /* 消费首步 move_right */
    bool nr = false;
    disp_tick(&d, 500, &nr); /* 产生连发命令 */
    CHECK(cq_count(&d.q) > 0, "repeating");
    while (cq_count(&d.q)) first_cmd(&d);
    CHECK(act_held_count(&d.act) == 1, "right held");

    /* 新策略到达：F11 之外把方向键映射换一版（仍合法），代次+1 */
    Policy p2;
    char *j2 = policy_default_payload_json(2);
    ValidationReport vr;
    CHECK(policy_validate_payload_text(j2, &p2, &vr), "p2 valid");
    free(j2);
    PolicyEnvelope env = {0}; env.generation = 2;
    uint32_t released = disp_stage_and_commit(&d, &p2, &env, 520);
    CHECK(released == 1, "synthetic release for held key, got %u", released);
    CHECK(act_held_count(&d.act) == 0, "no stuck key after boundary");
    CHECK(act_generation(&d.act) == 2, "generation advanced");
    CHECK(d.mods == 0, "mods cleared after boundary");

    /* 边界之后，旧物理键若仍按着而事件以"非重复 down"到来，
       按【新策略】解释一次；repeat 标志的陈旧事件被丢弃。 */
    key(&d, ORIGIN_HUMAN, NK_RIGHT, 0, true, true, false);
    CHECK(cq_count(&d.q) == 0, "stale repeat after boundary dropped");
    key(&d, ORIGIN_HUMAN, NK_RIGHT, 0, true, false, false);
    const char *c1 = first_cmd(&d);
    CHECK(c1 && strcmp(c1, "move_right") == 0,
          "fresh down under new policy works: %s", c1 ? c1 : "(null)");
    key(&d, ORIGIN_HUMAN, NK_RIGHT, 0, false, false, false);

    /* 修饰和弦必须重新按下：边界后仅按 Q 不应触发任何保留/命令 */
    key(&d, ORIGIN_HUMAN, NK_Q, 0, true, false, false);
    CHECK(cq_count(&d.q) == 0, "Q without Ctrl is harmless");
    key(&d, ORIGIN_HUMAN, NK_Q, 0, false, false, false);
}

/* ---------- 11. 安全闸：重放不能伪造业务确认 ---------- */
static void test_replay_safety(void) {
    SECTION("replay cannot forge business confirm");
    Policy p; policy_default(&p);
    JournalMem m; jm_init(&m);
    jm_write_header(&m, "dev-001", 1, "2026-10-04T10:00:00Z");
    InputEvent confirm = ie_key(ORIGIN_HUMAN, NK_RETURN, 0, true, false, false);
    jm_append_input(&m, 100, &confirm);
    InputEvent fs = ie_key(ORIGIN_HUMAN, NK_F11, 0, true, false, false);
    jm_append_input(&m, 200, &fs);
    InputEvent mv = ie_key(ORIGIN_HUMAN, NK_LEFT, 0, true, false, false);
    jm_append_input(&m, 300, &mv);
    jm_append_system(&m, 400, SYS_FOCUS_LOST, "alt-tab");

    ReplaySummary sum;
    Dispatcher rd;
    bool ok = replay_memory(m.data, m.len, &p, &sum, &rd);
    CHECK(ok, "replay completes clean");
    CHECK(!sum.corrupt, "no corruption");
    CHECK(sum.blocked_business == 1, "business confirm blocked: %u",
          sum.blocked_business);
    ReplayCmd cmds[32];
    uint32_t n = replay_drain(&rd, cmds, 32);
    bool saw_confirm = false, saw_fs = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (strcmp(cmds[i].name, "confirm_business") == 0) saw_confirm = true;
        if (strcmp(cmds[i].name, "toggle_fullscreen") == 0) saw_fs = true;
        CHECK(cmds[i].origin == ORIGIN_REPLAY, "all replayed cmds tagged");
    }
    CHECK(!saw_confirm, "no confirm_business in output");
    /* 全屏为边沿命令（headless 模式下 fs 回调为空，直接完成切换），
       它不入命令队列（由专用回调路径处理），因此只需验证确认被拦。 */
    (void)saw_fs;
    jm_free(&m);

    /* 损坏日志：改一字节 -> CRC 失败，重放中止 */
    JournalMem m2; jm_init(&m2);
    jm_write_header(&m2, "dev-001", 1, "t");
    InputEvent k = ie_key(ORIGIN_HUMAN, NK_A, 0, true, false, false);
    jm_append_input(&m2, 10, &k);
    /* 找到帧区域（header 之后），翻转一个数据字节 */
    m2.data[sizeof(JournalHeader) + 6] ^= 0xFF;
    ReplaySummary s2;
    CHECK(!replay_memory(m2.data, m2.len, &p, &s2, NULL), "corrupt detected");
    CHECK(s2.corrupt, "summary marks corrupt");
    jm_free(&m2);

    /* 文件头错误（被当作别的格式）也必须失败 */
    static const char garbage[] = "not a journal at all";
    ReplaySummary s3;
    CHECK(!replay_memory((const uint8_t *)garbage, sizeof garbage, &p,
                         &s3, NULL), "bad header rejected");
}

/* ---------- 12. 映射包损坏：保留旧策略 + 可靠退出 ---------- */
static void test_corrupt_bundle_keeps_old(void) {
    SECTION("corrupt bundle keeps previous generation");
    Policy p; policy_default(&p);
    Dispatcher d; disp_init_headless(&d, &p, 0);
    CHECK(act_generation(&d.act) == 1, "gen 1 active");
    ValidationReport r;
    Policy bogus;
    const char *bad = "{ this is not valid json";
    bool ok = policy_validate_payload_text(bad, &bogus, &r);
    CHECK(!ok, "bad bundle rejected at validation");
    CHECK(act_generation(&d.act) == 1, "still gen 1 after rejection");
    /* 映射全坏也能退出 */
    key(&d, ORIGIN_HUMAN, NK_Q, KM_CTRL, true, false, false);
    CHECK(d.quit_requested, "Ctrl+Q quits regardless");
}

int main(void) {
    test_keys();
    test_json();
    test_hashes();
    test_policy();
    test_envelope();
    test_layers_and_ime();
    test_fullscreen_edge();
    test_repeat_bounded();
    test_focus_lost();
    test_activation_boundary();
    test_replay_safety();
    test_corrupt_bundle_keeps_old();

    if (g_failures == 0) {
        printf("\nALL TESTS PASSED\n");
        return 0;
    }
    printf("\n%d CHECK(S) FAILED\n", g_failures);
    return 1;
}
