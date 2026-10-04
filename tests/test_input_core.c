#include "input_logger.h"
#include "replay_runner.h"
#include "input_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static InputEvent key_event(uint32_t t, CanonicalKey key, KeyPhase phase, uint16_t mods) {
    InputEvent e;
    input_event_init(&e, EV_KEY, t);
    e.key = key;
    e.phase = phase;
    e.modifiers = mods;
    return e;
}

static const InputEvent *key_ptr(uint32_t t, CanonicalKey key, KeyPhase phase, uint16_t mods) {
    static InputEvent event;
    event = key_event(t, key, phase, mods);
    return &event;
}

static OutputEvent only(InputCore *core, const InputEvent *event) {
    OutputList out;
    input_core_process(core, event, &out);
    assert(out.count <= 1);
    if (out.count == 0) {
        OutputEvent none = {0};
        none.type = OUT_IGNORE;
        return none;
    }
    return out.items[0];
}

static void test_fullscreen_edge_only(void) {
    InputCore core;
    input_core_init(&core, 1280, 720, TRUST_OPERATIONAL);

    OutputEvent e = only(&core, key_ptr(10, KEY_F11, PHASE_DOWN, MOD_NONE));
    assert(e.type == OUT_FULLSCREEN_TOGGLE && e.target_fullscreen);

    InputEvent repeat = key_event(20, KEY_F11, PHASE_REPEAT, MOD_NONE);
    OutputEvent up = only(&core, &repeat);
    assert(up.type == OUT_IGNORE);

    InputEvent second_down = key_event(30, KEY_F11, PHASE_DOWN, MOD_NONE);
    OutputEvent blocked = only(&core, &second_down);
    assert(blocked.type == OUT_IGNORE);

    InputEvent confirmed;
    input_event_init(&confirmed, EV_FULLSCREEN_STATE, 40);
    confirmed.fullscreen = true;
    OutputEvent confirmation = only(&core, &confirmed);
    assert(confirmation.type == OUT_IGNORE);
    assert(core.fullscreen);
    assert(only(&core, key_ptr(45, KEY_F11, PHASE_UP, MOD_NONE)).type == OUT_IGNORE);

    OutputEvent toggle_back = only(&core, key_ptr(50, KEY_F11, PHASE_DOWN, MOD_NONE));
    assert(toggle_back.type == OUT_FULLSCREEN_TOGGLE && !toggle_back.target_fullscreen);
}

static void test_bounded_arrow_repeat(void) {
    InputCore core;
    input_core_init(&core, 1280, 720, TRUST_OPERATIONAL);
    int x = core.marker_x;

    OutputEvent first = only(&core, key_ptr(0, KEY_LEFT, PHASE_DOWN, MOD_NONE));
    assert(first.type == OUT_MOVE && first.dx == -8);
    x += first.dx;

    InputEvent tick;
    input_event_init(&tick, EV_TICK, 179);
    OutputEvent no_repeat = only(&core, &tick);
    assert(no_repeat.type == OUT_IGNORE);

    input_event_init(&tick, EV_TICK, 180);
    OutputEvent second = only(&core, &tick);
    assert(second.type == OUT_MOVE);
    x += second.dx;

    input_event_init(&tick, EV_TICK, 224);
    OutputEvent gap = only(&core, &tick);
    assert(gap.type == OUT_IGNORE);

    input_event_init(&tick, EV_TICK, 225);
    OutputEvent third = only(&core, &tick);
    assert(third.type == OUT_MOVE);
    x += third.dx;
    assert(core.marker_x == x && core.marker_x >= 0);

    OutputEvent released = only(&core, key_ptr(230, KEY_LEFT, PHASE_UP, MOD_NONE));
    assert(released.type == OUT_IGNORE);
}

static void test_layer_and_ime_dispatch(void) {
    InputCore core;
    input_core_init(&core, 1280, 720, TRUST_OPERATIONAL);

    InputEvent modal;
    input_event_init(&modal, EV_LAYER, 0);
    modal.layer = LAYER_MODAL;
    assert(only(&core, &modal).type == OUT_IGNORE);
    assert(only(&core, key_ptr(1, KEY_F11, PHASE_DOWN, MOD_NONE)).type == OUT_IGNORE);
    OutputEvent cancel = only(&core, key_ptr(2, KEY_ESCAPE, PHASE_DOWN, MOD_NONE));
    assert(cancel.type == OUT_CANCEL_MODAL);

    InputEvent textbox;
    input_event_init(&textbox, EV_LAYER, 3);
    textbox.layer = LAYER_TEXTBOX;
    assert(only(&core, &textbox).type == OUT_IGNORE);
    assert(only(&core, key_ptr(4, KEY_RIGHT, PHASE_DOWN, MOD_NONE)).type == OUT_IGNORE);

    InputEvent composition;
    input_event_init(&composition, EV_COMPOSITION, 5);
    composition.composing = true;
    snprintf(composition.text, sizeof(composition.text), "zhong");
    assert(only(&core, &composition).type == OUT_IGNORE);

    InputEvent esc = key_event(6, KEY_ESCAPE, PHASE_DOWN, MOD_NONE);
    esc.composing = true;
    esc.layer = LAYER_TEXTBOX;
    assert(only(&core, &esc).type == OUT_IGNORE);

    input_event_init(&composition, EV_COMPOSITION, 7);
    composition.composing = false;
    assert(only(&core, &composition).type == OUT_IGNORE);
    cancel = only(&core, key_ptr(8, KEY_ESCAPE, PHASE_DOWN, MOD_NONE));
    assert(cancel.type == OUT_CANCEL_MODAL);

    OutputEvent quit = only(&core, key_ptr(9, KEY_Q, PHASE_DOWN, MOD_CTRL | MOD_SHIFT));
    assert(quit.type == OUT_SAFE_QUIT);
}

static void make_package(char *out, size_t cap, const char *payload) {
    char digest[SHA256_HEX_SIZE];
    sha256_hex(payload, strlen(payload), digest);
    size_t pos = (size_t)snprintf(out, cap,
             "{\"version\":1,\"generation\":1,\"payload\":\"");
    for (const char *p = payload; *p != '\0' && pos + 2 < cap; ++p) {
        if (*p == '"' || *p == '\\') {
            out[pos++] = '\\';
        }
        out[pos++] = *p;
    }
    pos += (size_t)snprintf(out + pos, cap - pos, "\",\"sha256\":\"%s\"}", digest);
    (void)pos;
}

static const char *V1_PAYLOAD =
    "{\"generation\":1,\"scene_width\":1280,\"scene_height\":720,"
    "\"repeat\":{\"initial_ms\":160,\"interval_ms\":40,\"step_pixels\":10},"
    "\"bindings\":["
    "{\"key\":\"Escape\",\"modifiers\":0,\"command\":\"modal.cancel\",\"alias\":false},"
    "{\"key\":\"F11\",\"modifiers\":0,\"command\":\"fullscreen.toggle\",\"alias\":false},"
    "{\"key\":\"ArrowLeft\",\"modifiers\":0,\"command\":\"move.left\",\"alias\":false},"
    "{\"key\":\"ArrowRight\",\"modifiers\":0,\"command\":\"move.right\",\"alias\":false},"
    "{\"key\":\"ArrowUp\",\"modifiers\":0,\"command\":\"move.up\",\"alias\":false},"
    "{\"key\":\"ArrowDown\",\"modifiers\":0,\"command\":\"move.down\",\"alias\":false},"
    "{\"key\":\"KeyB\",\"modifiers\":0,\"command\":\"background.restore\",\"alias\":false}]}";

static void parse_v1(InputPolicy *policy) {
    char package[4096];
    make_package(package, sizeof(package), V1_PAYLOAD);
    PolicyPackage pkg;
    char reason[INPUT_REASON_CAP];
    PolicyParseResult result = policy_package_parse(
        package, strlen(package), &pkg, policy, reason, sizeof(reason));
    assert(result == POLICY_PARSE_OK);
    policy_package_free(&pkg);
}

static void test_remote_policy_up_boundary(void) {
    InputCore core;
    input_core_init(&core, 1280, 720, TRUST_OPERATIONAL);
    assert(only(&core, key_ptr(0, KEY_LEFT, PHASE_DOWN, MOD_NONE)).type == OUT_MOVE);

    InputPolicy next;
    parse_v1(&next);
    OutputList out;
    assert(input_core_stage_policy(&core, &next, 1, "checksum", &out));
    assert(out.count == 1 && out.items[0].status == POLICY_STATUS_STAGED);
    assert(core.applied_generation == 0);

    InputEvent stage_event;
    input_event_init(&stage_event, EV_POLICY_STAGE, 10);
    input_core_process(&core, &stage_event, &out);
    assert(out.count == 0);
    assert(core.applied_generation == 0);

    input_core_process(&core, key_ptr(20, KEY_LEFT, PHASE_UP, MOD_NONE), &out);
    bool activated = false;
    for (int i = 0; i < out.count; ++i) {
        activated |= out.items[i].status == POLICY_STATUS_ACTIVATED;
    }
    assert(activated && core.applied_generation == 1);

    OutputEvent moved = only(&core, key_ptr(30, KEY_RIGHT, PHASE_DOWN, MOD_NONE));
    assert(moved.type == OUT_MOVE && moved.dx == 10);
}

static void test_blur_and_display_recovery(void) {
    InputCore core;
    input_core_init(&core, 1280, 720, TRUST_OPERATIONAL);
    InputPolicy next;
    parse_v1(&next);

    assert(only(&core, key_ptr(0, KEY_LEFT, PHASE_DOWN, MOD_NONE)).type == OUT_MOVE);
    OutputList out;
    input_core_stage_policy(&core, &next, 1, NULL, &out);
    assert(core.applied_generation == 0);

    InputEvent blur;
    input_event_init(&blur, EV_FOCUS, 10);
    blur.focused = false;
    input_core_process(&core, &blur, &out);
    bool activated = false;
    for (int i = 0; i < out.count; ++i) {
        activated |= out.items[i].status == POLICY_STATUS_ACTIVATED;
    }
    assert(activated);

    OutputEvent fs = only(&core, key_ptr(20, KEY_F11, PHASE_DOWN, MOD_NONE));
    assert(fs.type == OUT_FULLSCREEN_TOGGLE);
    InputEvent confirmed;
    input_event_init(&confirmed, EV_FULLSCREEN_STATE, 30);
    confirmed.fullscreen = true;
    assert(only(&core, &confirmed).type == OUT_IGNORE);

    InputEvent unplug;
    input_event_init(&unplug, EV_DISPLAY, 40);
    unplug.display_present = false;
    OutputEvent recover = only(&core, &unplug);
    assert(recover.type == OUT_FULLSCREEN_TOGGLE && !recover.target_fullscreen);
}

static void test_conflict_and_corrupt_package(void) {
    InputPolicy bad;
    input_policy_default(&bad, 1280, 720);
    bad.bindings[0].key = KEY_F11;
    char reason[INPUT_REASON_CAP];
    assert(!input_policy_validate(&bad, reason, sizeof(reason)));
    assert(strstr(reason, "duplicate") != NULL || strstr(reason, "protected") != NULL);

    char package[4096];
    make_package(package, sizeof(package), V1_PAYLOAD);
    char *checksum = strstr(package, "\"sha256\":\"");
    checksum[10] = checksum[10] == 'a' ? 'b' : 'a';
    PolicyPackage pkg;
    InputPolicy policy;
    PolicyParseResult result = policy_package_parse(
        package, strlen(package), &pkg, &policy, reason, sizeof(reason));
    assert(result == POLICY_PARSE_CHECKSUM_MISMATCH);
}

static void test_hash_chain_replay_sandbox(void) {
    const char *path = "/tmp/input-policy-test-log.jsonl";
    InputLogWriter writer;
    assert(input_log_open(&writer, path, "exp-1", TRUST_EXPERIMENT));
    assert(input_log_manifest(&writer, "device-test", "test"));
    InputEvent down = key_event(1, KEY_F11, PHASE_DOWN, MOD_NONE);
    assert(input_log_event(&writer, &down));
    assert(input_log_close(&writer));

    InputLogReader operational;
    assert(!input_replay_open(&operational, path, TRUST_OPERATIONAL));

    InputLogReader reader;
    assert(input_replay_open(&reader, path, TRUST_EXPERIMENT));
    ReplayedEvent record;
    assert(input_replay_next(&reader, &record));
    assert(record.event.type == EV_KEY && record.event.key == KEY_F11);
    InputCore replay_core;
    input_core_init(&replay_core, 1280, 720, TRUST_EXPERIMENT);
    OutputEvent replayed = only(&replay_core, &record.event);
    assert(replayed.type == OUT_FULLSCREEN_TOGGLE);
    input_replay_close(&reader);

    ReplaySummary replay_summary;
    char replay_error[INPUT_REASON_CAP];
    assert(input_replay_run(path, 1280, 720, &replay_summary,
                            replay_error, sizeof(replay_error)));
    assert(replay_summary.events == 1);
    assert(replay_summary.commands == 1);

    FILE *f = fopen(path, "r+b");
    assert(f != NULL);
    fseek(f, -4, SEEK_END);
    fputc('x', f);
    fclose(f);
    assert(input_replay_open(&reader, path, TRUST_EXPERIMENT));
    assert(!input_replay_next(&reader, &record));
    input_replay_close(&reader);
}

int main(void) {
    test_fullscreen_edge_only();
    test_bounded_arrow_repeat();
    test_layer_and_ime_dispatch();
    test_remote_policy_up_boundary();
    test_blur_and_display_recovery();
    test_conflict_and_corrupt_package();
    test_hash_chain_replay_sandbox();
    puts("input policy core tests passed");
    return 0;
}
