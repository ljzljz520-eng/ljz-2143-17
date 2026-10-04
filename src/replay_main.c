/*
 * replay_main.c - 本地日志重放工具（不打开窗口、不触发真实全屏/退出）
 *
 *   ./replay_tool <journal.islog> [generation-json-payload]
 *
 * 安全：重放命令的 origin 强制为 replay，confirm_business 被拦截并统计；
 * 它只能复现实验输入，无法伪造现场人员已执行的业务确认。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "core/dispatcher.h"
#include "core/json.h"
#include "core/policy.h"
#include "core/replay.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = 0;
    fclose(f);
    if (out_len) *out_len = (size_t)n;
    return b;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <journal.islog> [payload.json]\n", argv[0]);
        return 2;
    }
    Policy p;
    if (argc >= 3) {
        size_t n; char *j = read_file(argv[2], &n);
        if (!j) { perror("payload"); return 2; }
        ValidationReport r;
        if (!policy_validate_payload_text(j, &p, &r)) {
            fprintf(stderr, "payload invalid:\n");
            for (uint32_t i = 0; i < r.issue_count; ++i)
                fprintf(stderr, "  [%d] %s\n", r.issues[i].code,
                        r.issues[i].detail);
            free(j);
            return 2;
        }
        free(j);
    } else {
        policy_default(&p);
    }

    ReplaySummary s;
    Dispatcher d;
    bool ok = replay_file(argv[1], &p, &s, &d);
    printf("replay: %s\n", ok ? "OK" : "ABORTED (corrupt journal)");
    printf("  input frames : %u\n", s.frames_input);
    printf("  system frames: %u\n", s.frames_system);
    printf("  duration ms  : %u\n", s.duration_ms);
    printf("  commands     : %u\n", s.commands_emitted);
    printf("  BLOCKED business confirmations: %u\n", s.blocked_business);
    if (s.corrupt) printf("  reason: %s\n", s.corrupt_reason);

    ReplayCmd cmds[256];
    uint32_t n = replay_drain(&d, cmds, 256);
    printf("--- command sequence ---\n");
    for (uint32_t i = 0; i < n; ++i)
        printf("  %-22s origin=%s arg=%d\n", cmds[i].name,
               origin_name(cmds[i].origin), cmds[i].arg);
    return ok ? 0 : 1;
}
