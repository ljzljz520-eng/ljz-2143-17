#define _POSIX_C_SOURCE 200809L

#include "input_logger.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static void append_json_escaped(char *out, size_t cap, const char *value) {
    size_t pos = strlen(out);
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0' && pos + 2 < cap; ++p) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            if (pos + 2 >= cap) break;
            out[pos++] = '\\';
            out[pos++] = (char)c;
        } else if (c == '\n') {
            if (pos + 2 >= cap) break;
            out[pos++] = '\\';
            out[pos++] = 'n';
        } else if (c < 0x20 || c >= 0x7f) {
            if (pos + 7 >= cap) break;
            pos += (size_t)snprintf(out + pos, cap - pos, "\\u%04x", c);
        } else {
            out[pos++] = (char)c;
        }
    }
    if (pos >= cap) pos = cap - 1;
    out[pos] = '\0';
}

static void hash_chain_line(char prev[SHA256_HEX_SIZE], const char *line_without_hash,
                            char hash_out[SHA256_HEX_SIZE]) {
    char signed_input[4096];
    snprintf(signed_input, sizeof(signed_input), "%s|%s", prev, line_without_hash);
    sha256_hex(signed_input, strlen(signed_input), hash_out);
    snprintf(prev, SHA256_HEX_SIZE, "%s", hash_out);
}

static bool mkdir_parent(const char *path) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", path);
    char *slash = strrchr(copy, '/');
    if (slash == NULL) return true;
    *slash = '\0';
    if (copy[0] == '\0') return true;

    for (char *p = copy + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0750) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    return mkdir(copy, 0750) == 0 || errno == EEXIST;
}

bool input_log_open(InputLogWriter *writer, const char *path, const char *session_id,
                    TrustMode trust) {
    if (writer == NULL || path == NULL || session_id == NULL) return false;
    memset(writer, 0, sizeof(*writer));
    if (!mkdir_parent(path)) return false;
    writer->file = fopen(path, "wb");
    if (writer->file == NULL) return false;
    snprintf(writer->session_id, sizeof(writer->session_id), "%s", session_id);
    writer->trust = trust;
    writer->previous_hash[0] = '0';
    writer->previous_hash[1] = '\0';
    return true;
}

bool input_log_manifest(InputLogWriter *writer, const char *device_id, const char *version) {
    if (writer == NULL || writer->file == NULL) return false;
    char body[512];
    char line[1024];
    snprintf(body, sizeof(body),
             "\"seq\":0,\"session\":\"%s\",\"trust\":\"%s\",\"device\":\"",
             writer->session_id, writer->trust == TRUST_EXPERIMENT ? "experiment" : "operational");
    append_json_escaped(body, sizeof(body), device_id == NULL ? "" : device_id);
    size_t pos = strlen(body);
    snprintf(body + pos, sizeof(body) - pos, "\",\"version\":\"");
    append_json_escaped(body, sizeof(body), version == NULL ? "" : version);
    snprintf(body + strlen(body), sizeof(body) - strlen(body), "\"");

    char hash[SHA256_HEX_SIZE];
    hash_chain_line(writer->previous_hash, body, hash);
    snprintf(line, sizeof(line), "{%s,\"hash\":\"%s\"}\n", body, hash);
    return fputs(line, writer->file) != EOF && fflush(writer->file) == 0;
}

static const char *event_type_name(InputEventType type) {
    switch (type) {
        case EV_KEY: return "key";
        case EV_FOCUS: return "focus";
        case EV_COMPOSITION: return "composition";
        case EV_LAYER: return "layer";
        case EV_FULLSCREEN_STATE: return "fullscreen-state";
        case EV_DISPLAY: return "display";
        case EV_TICK: return "tick";
        case EV_POLICY_STAGE: return "policy-stage";
        case EV_POLICY_LOADED: return "policy-loaded";
        case EV_POLICY_REJECTED: return "policy-rejected";
        default: return "unknown";
    }
}

bool input_log_event(InputLogWriter *writer, const InputEvent *event) {
    if (writer == NULL || writer->file == NULL || event == NULL) return false;

    char body[3072];
    char line[4096];
    snprintf(body, sizeof(body),
             "\"seq\":%llu,\"type\":\"%s\",\"t\":%u",
             (unsigned long long)(writer->sequence + 1),
             event_type_name(event->type), event->timestamp_ms);

    if (event->type == EV_KEY) {
        size_t pos = strlen(body);
        snprintf(body + pos, sizeof(body) - pos,
                 ",\"key\":\"%s\",\"phase\":%u,\"mods\":%u,\"scancode\":%u,\"focus\":%s,\"composing\":%s,\"layer\":%u",
                 canonical_key_name(event->key), event->phase, event->modifiers,
                 event->scancode, event->focused ? "true" : "false",
                 event->composing ? "true" : "false", event->layer);
    } else if (event->type == EV_FOCUS) {
        snprintf(body + strlen(body), sizeof(body) - strlen(body), ",\"focus\":%s",
                 event->focused ? "true" : "false");
    } else if (event->type == EV_COMPOSITION) {
        snprintf(body + strlen(body), sizeof(body) - strlen(body), ",\"composing\":%s,\"text\":\"", event->composing ? "true" : "false");
        append_json_escaped(body, sizeof(body), event->text);
        snprintf(body + strlen(body), sizeof(body) - strlen(body), "\"");
    } else if (event->type == EV_LAYER) {
        snprintf(body + strlen(body), sizeof(body) - strlen(body), ",\"layer\":%u", event->layer);
    } else if (event->type == EV_FULLSCREEN_STATE) {
        snprintf(body + strlen(body), sizeof(body) - strlen(body), ",\"fullscreen\":%s",
                 event->fullscreen ? "true" : "false");
    } else if (event->type == EV_DISPLAY) {
        snprintf(body + strlen(body), sizeof(body) - strlen(body), ",\"display\":%s",
                 event->display_present ? "true" : "false");
    } else if (event->type == EV_POLICY_REJECTED || event->type == EV_POLICY_STAGE) {
        snprintf(body + strlen(body), sizeof(body) - strlen(body),
                 ",\"generation\":%u,\"text\":\"", event->generation);
        append_json_escaped(body, sizeof(body), event->text);
        snprintf(body + strlen(body), sizeof(body) - strlen(body), "\"");
    }

    writer->sequence++;
    char hash[SHA256_HEX_SIZE];
    hash_chain_line(writer->previous_hash, body, hash);
    snprintf(line, sizeof(line), "{%s,\"hash\":\"%s\"}\n", body, hash);
    return fputs(line, writer->file) != EOF && fflush(writer->file) == 0;
}

bool input_log_close(InputLogWriter *writer) {
    if (writer == NULL || writer->file == NULL) return false;
    bool ok = fclose(writer->file) == 0;
    writer->file = NULL;
    return ok;
}

static bool extract_string(const char *line, const char *field, char *out, size_t out_len) {
    char with_comma[80];
    char at_start[80];
    snprintf(with_comma, sizeof(with_comma), ",\"%s\":\"", field);
    snprintf(at_start, sizeof(at_start), "{\"%s\":\"", field);
    const char *start = strstr(line, with_comma);
    size_t value_marker_len = strlen(with_comma);
    if (start == NULL) {
        start = strstr(line, at_start);
        value_marker_len = strlen(at_start);
    }
    if (start == NULL) return false;
    start += value_marker_len;
    const char *end = strchr(start, '"');
    if (end == NULL || (size_t)(end - start) >= out_len) return false;
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return true;
}

static bool extract_unsigned(const char *line, const char *field, unsigned long long *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", field);
    const char *p = strstr(line, pattern);
    if (p == NULL) return false;
    p += strlen(pattern);
    if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *endptr = NULL;
    *out = strtoull(p, &endptr, 10);
    return errno == 0 && endptr != p;
}

static bool verify_line_hash(const char *line, char prev[SHA256_HEX_SIZE]) {
    const char *hash_field = strstr(line, ",\"hash\":\"");
    if (hash_field == NULL) hash_field = strstr(line, "{\"hash\":\"");
    if (hash_field == NULL) return false;
    const char *hash_start = hash_field + 9;
    char expected[SHA256_HEX_SIZE];
    memcpy(expected, hash_start, 64);
    expected[64] = '\0';

    char signed_prefix[4096];
    size_t body_len = (size_t)(hash_field - line);
    if (body_len + 2 >= sizeof(signed_prefix)) return false;
    memcpy(signed_prefix, line + 1, body_len - 1);
    signed_prefix[body_len - 1] = '\0';

    Sha256Context ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, prev, strlen(prev));
    sha256_update(&ctx, "|", 1);
    sha256_update(&ctx, signed_prefix, strlen(signed_prefix));
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_final(&ctx, digest);
    static const char hex_digits[] = "0123456789abcdef";
    char actual[SHA256_HEX_SIZE];
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        actual[i * 2] = hex_digits[digest[i] >> 4];
        actual[i * 2 + 1] = hex_digits[digest[i] & 0x0f];
    }
    actual[64] = '\0';
    if (strcmp(actual, expected) != 0) return false;
    snprintf(prev, SHA256_HEX_SIZE, "%s", expected);
    return true;
}

bool input_replay_open(InputLogReader *reader, const char *path, TrustMode required_trust) {
    if (reader == NULL || path == NULL) return false;
    memset(reader, 0, sizeof(*reader));
    reader->file = fopen(path, "rb");
    if (reader->file == NULL) return false;
    reader->previous_hash[0] = '0';
    reader->previous_hash[1] = '\0';

    char *line = fgets(reader->line, (int)sizeof(reader->line), reader->file);
    if (line == NULL || !verify_line_hash(line, reader->previous_hash)) {
        fclose(reader->file);
        reader->file = NULL;
        return false;
    }

    char trust[32];
    char session[LOG_SESSION_CAP];
    if (!extract_string(line, "trust", trust, sizeof(trust)) ||
        !extract_string(line, "session", session, sizeof(session))) {
        fclose(reader->file);
        reader->file = NULL;
        return false;
    }
    TrustMode actual = strcmp(trust, "experiment") == 0 ? TRUST_EXPERIMENT : TRUST_OPERATIONAL;
    if (actual != required_trust || actual != TRUST_EXPERIMENT) {
        fclose(reader->file);
        reader->file = NULL;
        return false;
    }
    snprintf(reader->session_id, sizeof(reader->session_id), "%s", session);
    reader->trust = actual;
    return true;
}

static bool parse_key_event(const char *line, InputEvent *event) {
    char key[32];
    char composing[8] = "false";
    char focus[8] = "true";
    unsigned long long phase = 0;
    unsigned long long mods = 0;
    unsigned long long scancode = 0;
    unsigned long long layer = 0;
    if (!extract_string(line, "key", key, sizeof(key))) return false;
    if (!canonical_key_from_name(key, &event->key)) return false;
    if (!extract_unsigned(line, "phase", &phase)) return false;
    if (!extract_unsigned(line, "mods", &mods)) return false;
    if (!extract_unsigned(line, "scancode", &scancode)) return false;
    if (!extract_unsigned(line, "layer", &layer)) return false;
    extract_string(line, "composing", composing, sizeof(composing));
    extract_string(line, "focus", focus, sizeof(focus));
    event->phase = (KeyPhase)phase;
    event->modifiers = (uint16_t)mods;
    event->scancode = (uint32_t)scancode;
    event->layer = (InputLayer)layer;
    event->composing = strcmp(composing, "true") == 0;
    event->focused = strcmp(focus, "true") == 0;
    return true;
}

bool input_replay_next(InputLogReader *reader, ReplayedEvent *out) {
    if (reader == NULL || reader->file == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));

    char *line = fgets(reader->line, (int)sizeof(reader->line), reader->file);
    if (line == NULL) return false;
    if (!verify_line_hash(line, reader->previous_hash)) return false;

    unsigned long long seq = 0;
    unsigned long long timestamp = 0;
    char type[32];
    if (!extract_unsigned(line, "seq", &seq) ||
        !extract_unsigned(line, "t", &timestamp) ||
        !extract_string(line, "type", type, sizeof(type))) {
        return false;
    }

    InputEvent event;
    input_event_init(&event, EV_TICK, (uint32_t)timestamp);

    if (strcmp(type, "key") == 0) {
        event.type = EV_KEY;
        if (!parse_key_event(line, &event)) return false;
    } else if (strcmp(type, "focus") == 0) {
        event.type = EV_FOCUS;
        char value[8] = "true";
        extract_string(line, "focus", value, sizeof(value));
        event.focused = strcmp(value, "true") == 0;
    } else if (strcmp(type, "composition") == 0) {
        event.type = EV_COMPOSITION;
        char value[8] = "false";
        extract_string(line, "composing", value, sizeof(value));
        event.composing = strcmp(value, "true") == 0;
        extract_string(line, "text", event.text, sizeof(event.text));
    } else if (strcmp(type, "layer") == 0) {
        event.type = EV_LAYER;
        unsigned long long layer = 0;
        if (!extract_unsigned(line, "layer", &layer)) return false;
        event.layer = (InputLayer)layer;
    } else if (strcmp(type, "fullscreen-state") == 0) {
        event.type = EV_FULLSCREEN_STATE;
        char value[8] = "false";
        extract_string(line, "fullscreen", value, sizeof(value));
        event.fullscreen = strcmp(value, "true") == 0;
    } else if (strcmp(type, "display") == 0) {
        event.type = EV_DISPLAY;
        char value[8] = "true";
        extract_string(line, "display", value, sizeof(value));
        event.display_present = strcmp(value, "true") == 0;
    } else if (strcmp(type, "tick") == 0) {
        event.type = EV_TICK;
    } else {
        return false;
    }

    out->sequence = seq;
    out->timestamp_ms = (uint32_t)timestamp;
    snprintf(out->type, sizeof(out->type), "%s", type);
    out->event = event;
    out->has_event = true;
    return true;
}

void input_replay_close(InputLogReader *reader) {
    if (reader == NULL || reader->file == NULL) return;
    fclose(reader->file);
    reader->file = NULL;
}
