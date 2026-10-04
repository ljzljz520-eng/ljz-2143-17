#include "replay_runner.h"

#include <stdio.h>
#include <string.h>

bool input_replay_run(
    const char *log_path,
    int scene_width,
    int scene_height,
    ReplaySummary *summary,
    char *error,
    size_t error_len
) {
    if (summary != NULL) memset(summary, 0, sizeof(*summary));
    if (error != NULL && error_len > 0) error[0] = '\0';

    InputLogReader reader;
    if (!input_replay_open(&reader, log_path, TRUST_EXPERIMENT)) {
        snprintf(error, error_len, "log is not an intact experiment hash chain");
        return false;
    }

    InputCore core;
    input_core_init(&core, scene_width, scene_height, TRUST_EXPERIMENT);
    ReplayedEvent record;
    while (input_replay_next(&reader, &record)) {
        if (summary != NULL) summary->events++;

        /* The log format has no operational command field; any future addition is denied
         * before dispatch so experiment replay cannot impersonate a worker confirmation. */
        if (strstr(reader.line, "business.confirm") != NULL ||
            strstr(reader.line, "operator_confirmed") != NULL) {
            if (summary != NULL) summary->blocked_business_confirmations++;
            input_replay_close(&reader);
            snprintf(error, error_len, "business confirmation cannot be replayed");
            return false;
        }

        OutputList outputs;
        input_core_process(&core, &record.event, &outputs);
        if (summary != NULL) {
            for (int i = 0; i < outputs.count; ++i) {
                if (outputs.items[i].type != OUT_IGNORE) summary->commands++;
            }
            summary->final_generation = core.applied_generation;
        }
    }

    if (summary != NULL) summary->final_generation = core.applied_generation;
    input_replay_close(&reader);
    return true;
}
