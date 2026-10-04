#ifndef INPUT_LOGGER_H
#define INPUT_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "input_policy.h"

#define LOG_SESSION_CAP 64

typedef struct {
    FILE *file;
    char session_id[LOG_SESSION_CAP];
    TrustMode trust;
    uint64_t sequence;
    char previous_hash[SHA256_HEX_SIZE];
} InputLogWriter;

typedef struct {
    uint64_t sequence;
    uint32_t timestamp_ms;
    char type[32];
    InputEvent event;
    bool has_event;
} ReplayedEvent;

typedef struct {
    FILE *file;
    char session_id[LOG_SESSION_CAP];
    TrustMode trust;
    uint64_t sequence;
    char previous_hash[SHA256_HEX_SIZE];
    char line[4096];
} InputLogReader;

bool input_log_open(
    InputLogWriter *writer,
    const char *path,
    const char *session_id,
    TrustMode trust
);
bool input_log_manifest(InputLogWriter *writer, const char *device_id, const char *version);
bool input_log_event(InputLogWriter *writer, const InputEvent *event);
bool input_log_close(InputLogWriter *writer);

bool input_replay_open(InputLogReader *reader, const char *path, TrustMode required_trust);
bool input_replay_next(InputLogReader *reader, ReplayedEvent *out);
void input_replay_close(InputLogReader *reader);

#endif
