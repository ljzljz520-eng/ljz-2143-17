#ifndef REPLAY_RUNNER_H
#define REPLAY_RUNNER_H

#include <stdbool.h>
#include "input_logger.h"
#include "input_policy.h"

typedef struct {
    uint64_t events;
    uint64_t commands;
    uint64_t blocked_business_confirmations;
    uint64_t final_generation;
} ReplaySummary;

bool input_replay_run(
    const char *log_path,
    int scene_width,
    int scene_height,
    ReplaySummary *summary,
    char *error,
    size_t error_len
);

#endif
