#include "activation.h"

#include <string.h>

void act_init(Activation *a) {
    memset(a, 0, sizeof *a);
}

void act_load_initial(Activation *a, const Policy *p) {
    memset(a, 0, sizeof *a);
    a->current = *p;
    a->has_current = true;
}

void act_stage(Activation *a, const Policy *p, const PolicyEnvelope *env) {
    a->staged_policy = *p;
    if (env) a->staged_env = *env;
    a->staged = true;
}

bool act_has_staged(const Activation *a) { return a->staged; }

uint32_t act_commit_prepare(Activation *a) {
    if (!a->staged) return 0;
    a->release_count = 0;
    a->mods_to_clear = KM_SHIFT | KM_CTRL | KM_ALT | KM_GUI;
    for (int i = 0; i < ACT_HELD_KEYS_MAX; ++i) {
        if (a->held_down[i]) {
            if (a->release_count < ACT_HELD_KEYS_MAX)
                a->release_list[a->release_count++] = a->held[i];
            a->held_down[i] = false;
        }
    }
    return a->release_count;
}

void act_commit_finish(Activation *a) {
    if (!a->staged) return;
    a->current = a->staged_policy;
    a->has_current = true;
    a->staged = false;
    memset(&a->staged_policy, 0, sizeof a->staged_policy);
    a->release_count = 0;
    a->mods_to_clear = 0;
}


void act_mark_down(Activation *a, NK_Key key, uint32_t mods) {
    (void)mods;
    for (int i = 0; i < ACT_HELD_KEYS_MAX; ++i)
        if (a->held_down[i] && a->held[i] == key) return;
    for (int i = 0; i < ACT_HELD_KEYS_MAX; ++i) {
        if (!a->held_down[i]) {
            a->held[i] = key;
            a->held_down[i] = true;
            return;
        }
    }
}

void act_mark_up(Activation *a, NK_Key key) {
    for (int i = 0; i < ACT_HELD_KEYS_MAX; ++i)
        if (a->held_down[i] && a->held[i] == key)
            a->held_down[i] = false;
}

bool act_is_down(Activation *a, NK_Key key) {
    for (int i = 0; i < ACT_HELD_KEYS_MAX; ++i)
        if (a->held_down[i] && a->held[i] == key) return true;
    return false;
}

uint32_t act_held_count(Activation *a) {
    uint32_t n = 0;
    for (int i = 0; i < ACT_HELD_KEYS_MAX; ++i)
        if (a->held_down[i]) n++;
    return n;
}

const Policy *act_current(const Activation *a) {
    return a->has_current ? &a->current : NULL;
}

uint64_t act_generation(const Activation *a) {
    return a->has_current ? a->current.generation : 0;
}
