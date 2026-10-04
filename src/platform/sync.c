#define _POSIX_C_SOURCE 200809L

#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/json.h"
#include "../core/policy.h"
#include "net.h"

static SyncConfig g_cfg;
static SyncStats g_stats;

void sync_init(SyncConfig cfg, SyncStats *stats) {
    g_cfg = cfg;
    memset(&g_stats, 0, sizeof g_stats);
    if (stats) *stats = g_stats;
}

const SyncStats *sync_stats(void) { return &g_stats; }

static void post_receipt(uint64_t generation, const char *status,
                         const char *reason, uint32_t released) {
    char body[512];
    snprintf(body, sizeof body,
        "{\"generation\":%llu,\"status\":\"%s\",\"reason\":\"%s\","
        "\"released_keys\":%u}",
        (unsigned long long)generation, status,
        reason ? reason : "", released);
    char url[512];
    snprintf(url, sizeof url, "%s/api/devices/%s/receipts",
             g_cfg.base_url, g_cfg.device_id);
    HttpResponse r;
    if (http_post_json(url, body, &r)) http_response_free(&r);
}

bool sync_poll(Dispatcher *d, uint32_t now_ms, uint32_t *released_out) {
    if (released_out) *released_out = 0;
    g_stats.polls++;

    char url[512];
    snprintf(url, sizeof url, "%s/api/devices/%s/policy",
             g_cfg.base_url, g_cfg.device_id);

    HttpResponse resp;
    if (!http_get(url, &resp)) {
        g_stats.network_errors++;
        return false; /* 保留 last-known-good */
    }
    if (resp.status != 200) {
        http_response_free(&resp);
        g_stats.network_errors++;
        return false;
    }

    PolicyEnvelope env;
    char *canon = NULL;
    ValidationReport er;
    bool envelope_ok = policy_parse_envelope(resp.data, &env, &canon, &er);
    http_response_free(&resp);

    if (!envelope_ok) {
        g_stats.rejected++;
        char why[160] = "envelope validation failed";
        if (er.issue_count)
            snprintf(why, sizeof why, "%s", er.issues[0].detail);
        post_receipt(env.generation ? env.generation : 0, "rejected", why, 0);
        return false;
    }

    Policy p;
    ValidationReport pr;
    bool policy_ok = policy_validate_payload_text(canon, &p, &pr);
    free(canon);
    if (!policy_ok) {
        g_stats.rejected++;
        char why[160] = "payload validation failed";
        if (pr.issue_count)
            snprintf(why, sizeof why, "%s", pr.issues[0].detail);
        post_receipt(env.generation, "rejected", why, 0);
        return false;
    }

    if (env.generation <= act_generation(&d->act)) {
        g_stats.last_seen_generation = env.generation;
        return true; /* 旧代次/相同代次：不重复激活 */
    }

    uint32_t released = disp_stage_and_commit(d, &p, &env, now_ms);
    if (released_out) *released_out = released;
    g_stats.applied++;
    g_stats.last_seen_generation = env.generation;
    post_receipt(env.generation, "applied", "", released);
    return true;
}
