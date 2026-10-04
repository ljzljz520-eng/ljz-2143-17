#include "input_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARKER_SIZE 72
#define POLICY_VERSION 1
#define SAFE_QUIT_TIMEOUT_MS 500

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
typedef struct {
    char *key;
    JsonValue *value;
} JsonMember;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            size_t count;
        } array;
        struct {
            JsonMember *members;
            size_t count;
        } object;
    } u;
};

typedef struct {
    const char *p;
    const char *end;
    int error;
} JsonParser;

static JsonValue *json_parse_value(JsonParser *parser);

static void json_free(JsonValue *value) {
    if (value == NULL) {
        return;
    }

    if (value->type == JSON_STRING) {
        free(value->u.string);
    } else if (value->type == JSON_ARRAY) {
        for (size_t i = 0; i < value->u.array.count; ++i) {
            json_free(value->u.array.items[i]);
        }
        free(value->u.array.items);
    } else if (value->type == JSON_OBJECT) {
        for (size_t i = 0; i < value->u.object.count; ++i) {
            free(value->u.object.members[i].key);
            json_free(value->u.object.members[i].value);
        }
        free(value->u.object.members);
    }
    free(value);
}

static void json_skip_ws(JsonParser *parser) {
    while (parser->p < parser->end &&
           (*parser->p == ' ' || *parser->p == '\t' ||
            *parser->p == '\n' || *parser->p == '\r')) {
        parser->p++;
    }
}

static JsonValue *json_new(JsonType type) {
    JsonValue *value = calloc(1, sizeof(*value));
    if (value != NULL) {
        value->type = type;
    }
    return value;
}

static char *json_parse_string_raw(JsonParser *parser) {
    if (parser->p >= parser->end || *parser->p != '"') {
        parser->error = 1;
        return NULL;
    }
    parser->p++;

    size_t capacity = 32;
    size_t length = 0;
    char *out = malloc(capacity);
    if (out == NULL) {
        parser->error = 1;
        return NULL;
    }

    while (parser->p < parser->end && *parser->p != '"') {
        char c = *parser->p++;
        unsigned int hex = 0;

        if (c == '\0' || c == '\n' || c == '\r') {
            free(out);
            parser->error = 1;
            return NULL;
        }

        if (c != '\\') {
            if (length + 1 >= capacity) {
                capacity *= 2;
                char *grown = realloc(out, capacity);
                if (grown == NULL) {
                    free(out);
                    parser->error = 1;
                    return NULL;
                }
                out = grown;
            }
            out[length++] = c;
            continue;
        }

        if (parser->p >= parser->end) {
            break;
        }
        char esc = *parser->p++;
        char decoded = '?';
        switch (esc) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u':
                if ((size_t)(parser->end - parser->p) < 4) {
                    free(out);
                    parser->error = 1;
                    return NULL;
                }
                for (int i = 0; i < 4; ++i) {
                    char h = *parser->p++;
                    hex <<= 4;
                    if (h >= '0' && h <= '9') hex |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') hex |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') hex |= (unsigned)(h - 'A' + 10);
                    else {
                        free(out);
                        parser->error = 1;
                        return NULL;
                    }
                }
                if (hex >= 0x80) {
                    /* Policy payloads are UTF-8; escaped non-ASCII is rejected to keep checksum simple. */
                    free(out);
                    parser->error = 1;
                    return NULL;
                }
                decoded = (char)hex;
                break;
            default:
                free(out);
                parser->error = 1;
                return NULL;
        }

        if (length + 1 >= capacity) {
            capacity *= 2;
            char *grown = realloc(out, capacity);
            if (grown == NULL) {
                free(out);
                parser->error = 1;
                return NULL;
            }
            out = grown;
        }
        out[length++] = decoded;
    }

    if (parser->p >= parser->end || *parser->p != '"') {
        free(out);
        parser->error = 1;
        return NULL;
    }
    parser->p++;
    out[length] = '\0';
    return out;
}

static JsonValue *json_parse_string(JsonParser *parser) {
    char *text = json_parse_string_raw(parser);
    if (text == NULL) {
        return NULL;
    }
    JsonValue *value = json_new(JSON_STRING);
    if (value == NULL) {
        free(text);
        return NULL;
    }
    value->u.string = text;
    return value;
}

static JsonValue *json_parse_number(JsonParser *parser) {
    const char *start = parser->p;
    if (*parser->p == '-') parser->p++;
    while (parser->p < parser->end && *parser->p >= '0' && *parser->p <= '9') parser->p++;
    if (parser->p < parser->end && (*parser->p == '.' || *parser->p == 'e' || *parser->p == 'E')) {
        parser->error = 1;
        return NULL;
    }
    char buffer[32];
    size_t len = (size_t)(parser->p - start);
    if (len == 0 || len >= sizeof(buffer)) {
        parser->error = 1;
        return NULL;
    }
    memcpy(buffer, start, len);
    buffer[len] = '\0';
    JsonValue *value = json_new(JSON_NUMBER);
    if (value == NULL) {
        return NULL;
    }
    value->u.number = (double)strtoll(buffer, NULL, 10);
    return value;
}

static JsonValue *json_parse_array(JsonParser *parser) {
    parser->p++;
    JsonValue *value = json_new(JSON_ARRAY);
    if (value == NULL) {
        return NULL;
    }
    size_t capacity = 0;

    json_skip_ws(parser);
    if (parser->p < parser->end && *parser->p == ']') {
        parser->p++;
        return value;
    }

    for (;;) {
        json_skip_ws(parser);
        JsonValue *child = json_parse_value(parser);
        if (child == NULL) {
            json_free(value);
            return NULL;
        }
        if (value->u.array.count == capacity) {
            capacity = capacity ? capacity * 2 : 4;
            JsonValue **grown = realloc(value->u.array.items, capacity * sizeof(*grown));
            if (grown == NULL) {
                json_free(child);
                json_free(value);
                parser->error = 1;
                return NULL;
            }
            value->u.array.items = grown;
        }
        value->u.array.items[value->u.array.count++] = child;

        json_skip_ws(parser);
        if (parser->p >= parser->end) {
            json_free(value);
            parser->error = 1;
            return NULL;
        }
        if (*parser->p == ',') {
            parser->p++;
            continue;
        }
        if (*parser->p == ']') {
            parser->p++;
            return value;
        }
        json_free(value);
        parser->error = 1;
        return NULL;
    }
}

static JsonValue *json_parse_object(JsonParser *parser) {
    parser->p++;
    JsonValue *value = json_new(JSON_OBJECT);
    if (value == NULL) {
        return NULL;
    }
    size_t capacity = 0;

    json_skip_ws(parser);
    if (parser->p < parser->end && *parser->p == '}') {
        parser->p++;
        return value;
    }

    for (;;) {
        json_skip_ws(parser);
        char *key = json_parse_string_raw(parser);
        if (key == NULL) {
            json_free(value);
            return NULL;
        }
        json_skip_ws(parser);
        if (parser->p >= parser->end || *parser->p != ':') {
            free(key);
            json_free(value);
            parser->error = 1;
            return NULL;
        }
        parser->p++;
        json_skip_ws(parser);
        JsonValue *child = json_parse_value(parser);
        if (child == NULL) {
            free(key);
            json_free(value);
            return NULL;
        }

        if (value->u.object.count == capacity) {
            capacity = capacity ? capacity * 2 : 4;
            JsonMember *grown = realloc(value->u.object.members, capacity * sizeof(*grown));
            if (grown == NULL) {
                free(key);
                json_free(child);
                json_free(value);
                parser->error = 1;
                return NULL;
            }
            value->u.object.members = grown;
        }
        value->u.object.members[value->u.object.count].key = key;
        value->u.object.members[value->u.object.count].value = child;
        value->u.object.count++;

        json_skip_ws(parser);
        if (parser->p >= parser->end) {
            json_free(value);
            parser->error = 1;
            return NULL;
        }
        if (*parser->p == ',') {
            parser->p++;
            continue;
        }
        if (*parser->p == '}') {
            parser->p++;
            return value;
        }
        json_free(value);
        parser->error = 1;
        return NULL;
    }
}

static JsonValue *json_parse_value(JsonParser *parser) {
    json_skip_ws(parser);
    if (parser->p >= parser->end) {
        parser->error = 1;
        return NULL;
    }

    switch (*parser->p) {
        case '{': return json_parse_object(parser);
        case '[': return json_parse_array(parser);
        case '"': return json_parse_string(parser);
        case 't':
            if ((size_t)(parser->end - parser->p) >= 4 && memcmp(parser->p, "true", 4) == 0) {
                parser->p += 4;
                JsonValue *v = json_new(JSON_BOOL);
                if (v != NULL) v->u.boolean = true;
                return v;
            }
            break;
        case 'f':
            if ((size_t)(parser->end - parser->p) >= 5 && memcmp(parser->p, "false", 5) == 0) {
                parser->p += 5;
                JsonValue *v = json_new(JSON_BOOL);
                if (v != NULL) v->u.boolean = false;
                return v;
            }
            break;
        case 'n':
            if ((size_t)(parser->end - parser->p) >= 4 && memcmp(parser->p, "null", 4) == 0) {
                parser->p += 4;
                return json_new(JSON_NULL);
            }
            break;
        default:
            if (*parser->p == '-' || (*parser->p >= '0' && *parser->p <= '9')) {
                return json_parse_number(parser);
            }
            break;
    }
    parser->error = 1;
    return NULL;
}

static JsonValue *json_parse(const char *text, size_t len) {
    JsonParser parser = { .p = text, .end = text + len, .error = 0 };
    JsonValue *root = json_parse_value(&parser);
    json_skip_ws(&parser);
    if (parser.error || root == NULL || parser.p != parser.end) {
        json_free(root);
        return NULL;
    }
    return root;
}

static JsonValue *json_object_get(const JsonValue *object, const char *key) {
    if (object == NULL || object->type != JSON_OBJECT) {
        return NULL;
    }
    for (size_t i = 0; i < object->u.object.count; ++i) {
        if (strcmp(object->u.object.members[i].key, key) == 0) {
            return object->u.object.members[i].value;
        }
    }
    return NULL;
}

const char *canonical_key_name(CanonicalKey key) {
    switch (key) {
        case KEY_ESCAPE: return "Escape";
        case KEY_F11: return "F11";
        case KEY_LEFT: return "ArrowLeft";
        case KEY_RIGHT: return "ArrowRight";
        case KEY_UP: return "ArrowUp";
        case KEY_DOWN: return "ArrowDown";
        case KEY_R: return "KeyR";
        case KEY_B: return "KeyB";
        case KEY_BACKSPACE: return "Backspace";
        case KEY_Q: return "KeyQ";
        case KEY_W: return "KeyW";
        case KEY_A: return "KeyA";
        case KEY_S: return "KeyS";
        case KEY_D: return "KeyD";
        default: return "None";
    }
}

const char *command_name(CommandId command) {
    switch (command) {
        case CMD_CANCEL_MODAL: return "modal.cancel";
        case CMD_TOGGLE_FULLSCREEN: return "fullscreen.toggle";
        case CMD_MOVE_LEFT: return "move.left";
        case CMD_MOVE_RIGHT: return "move.right";
        case CMD_MOVE_UP: return "move.up";
        case CMD_MOVE_DOWN: return "move.down";
        case CMD_RESTORE_BACKGROUND: return "background.restore";
        case CMD_SAFE_QUIT: return "app.safe-quit";
        default: return "none";
    }
}

bool canonical_key_from_name(const char *name, CanonicalKey *key) {
    if (name == NULL || key == NULL) return false;
    for (int i = 1; i < KEY_COUNT; ++i) {
        CanonicalKey candidate = (CanonicalKey)i;
        if (strcmp(name, canonical_key_name(candidate)) == 0) {
            *key = candidate;
            return true;
        }
    }
    return false;
}

bool command_from_name(const char *name, CommandId *command) {
    if (name == NULL || command == NULL) return false;
    for (int i = 1; i < CMD_COUNT; ++i) {
        CommandId candidate = (CommandId)i;
        if (strcmp(name, command_name(candidate)) == 0) {
            *command = candidate;
            return true;
        }
    }
    return false;
}

static bool is_protected_key(CanonicalKey key) {
    return key == KEY_ESCAPE || key == KEY_F11 ||
           key == KEY_LEFT || key == KEY_RIGHT ||
           key == KEY_UP || key == KEY_DOWN;
}

static bool is_move_command(CommandId command) {
    return command == CMD_MOVE_LEFT || command == CMD_MOVE_RIGHT ||
           command == CMD_MOVE_UP || command == CMD_MOVE_DOWN;
}

static CommandId protected_command_for(CanonicalKey key) {
    switch (key) {
        case KEY_ESCAPE: return CMD_CANCEL_MODAL;
        case KEY_F11: return CMD_TOGGLE_FULLSCREEN;
        case KEY_LEFT: return CMD_MOVE_LEFT;
        case KEY_RIGHT: return CMD_MOVE_RIGHT;
        case KEY_UP: return CMD_MOVE_UP;
        case KEY_DOWN: return CMD_MOVE_DOWN;
        default: return CMD_NONE;
    }
}

static void set_reason(char *reason, size_t len, const char *message) {
    if (reason != NULL && len > 0) {
        snprintf(reason, len, "%s", message);
    }
}

static void add_binding(InputPolicy *policy, CanonicalKey key, uint16_t modifiers,
                        CommandId command, bool alias) {
    if (policy->binding_count >= INPUT_MAX_BINDINGS) return;
    KeyBinding *b = &policy->bindings[policy->binding_count++];
    b->key = key;
    b->modifiers = modifiers;
    b->command = command;
    b->protected_key = !alias && is_protected_key(key) &&
                       protected_command_for(key) == command;
    b->alias = alias;
}

void input_policy_default(InputPolicy *policy, int width, int height) {
    if (policy == NULL) return;
    memset(policy, 0, sizeof(*policy));
    policy->generation = 0;
    policy->repeat_initial_ms = 180;
    policy->repeat_interval_ms = 45;
    policy->step_pixels = 8;
    policy->scene_width = width > 0 ? width : 1280;
    policy->scene_height = height > 0 ? height : 720;

    add_binding(policy, KEY_ESCAPE, MOD_NONE, CMD_CANCEL_MODAL, false);
    add_binding(policy, KEY_F11, MOD_NONE, CMD_TOGGLE_FULLSCREEN, false);
    add_binding(policy, KEY_LEFT, MOD_NONE, CMD_MOVE_LEFT, false);
    add_binding(policy, KEY_RIGHT, MOD_NONE, CMD_MOVE_RIGHT, false);
    add_binding(policy, KEY_UP, MOD_NONE, CMD_MOVE_UP, false);
    add_binding(policy, KEY_DOWN, MOD_NONE, CMD_MOVE_DOWN, false);
    add_binding(policy, KEY_R, MOD_NONE, CMD_RESTORE_BACKGROUND, false);
}

static bool has_command(const InputPolicy *policy, CommandId command) {
    for (int i = 0; i < policy->binding_count; ++i) {
        if (policy->bindings[i].command == command) return true;
    }
    return false;
}

bool input_policy_validate(const InputPolicy *policy, char *reason, size_t reason_len) {
    if (policy == NULL) {
        set_reason(reason, reason_len, "policy is null");
        return false;
    }
    if (policy->binding_count < 7 || policy->binding_count > INPUT_MAX_BINDINGS) {
        set_reason(reason, reason_len, "binding count out of range");
        return false;
    }
    if (policy->repeat_initial_ms < 50 || policy->repeat_initial_ms > 2000 ||
        policy->repeat_interval_ms < 10 || policy->repeat_interval_ms > 500 ||
        policy->repeat_initial_ms < policy->repeat_interval_ms) {
        set_reason(reason, reason_len, "repeat timing out of range");
        return false;
    }
    if (policy->step_pixels < 1 || policy->step_pixels > 128 ||
        policy->scene_width <= MARKER_SIZE || policy->scene_height <= MARKER_SIZE) {
        set_reason(reason, reason_len, "step or scene dimensions are invalid");
        return false;
    }

    const CommandId required[] = {
        CMD_CANCEL_MODAL, CMD_TOGGLE_FULLSCREEN, CMD_MOVE_LEFT,
        CMD_MOVE_RIGHT, CMD_MOVE_UP, CMD_MOVE_DOWN
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!has_command(policy, required[i])) {
            set_reason(reason, reason_len, "a protected role is missing");
            return false;
        }
    }

    int restore_count = 0;
    bool alias_seen[CMD_COUNT] = {false};
    for (int i = 0; i < policy->binding_count; ++i) {
        const KeyBinding *b = &policy->bindings[i];
        if (b->key == KEY_NONE || b->key >= KEY_COUNT ||
            b->command == CMD_NONE || b->command >= CMD_COUNT) {
            set_reason(reason, reason_len, "unknown key or command");
            return false;
        }
        if (b->command == CMD_SAFE_QUIT) {
            set_reason(reason, reason_len, "Ctrl+Shift+Q is a reserved non-configurable chord");
            return false;
        }
        for (int j = 0; j < i; ++j) {
            if (policy->bindings[j].key == b->key &&
                policy->bindings[j].modifiers == b->modifiers) {
                set_reason(reason, reason_len, "duplicate physical key binding");
                return false;
            }
        }

        if (is_protected_key(b->key)) {
            if (b->modifiers != MOD_NONE || protected_command_for(b->key) != b->command) {
                set_reason(reason, reason_len, "protected key cannot be remapped");
                return false;
            }
        } else if (is_move_command(b->command)) {
            if (b->modifiers != MOD_NONE ||
                ((b->command == CMD_MOVE_LEFT) != (b->key == KEY_A) &&
                 (b->command == CMD_MOVE_RIGHT) != (b->key == KEY_D) &&
                 (b->command == CMD_MOVE_UP) != (b->key == KEY_W) &&
                 (b->command == CMD_MOVE_DOWN) != (b->key == KEY_S))) {
                set_reason(reason, reason_len, "unsupported movement alias");
                return false;
            }
            if (alias_seen[b->command]) {
                set_reason(reason, reason_len, "duplicate command binding");
                return false;
            }
            alias_seen[b->command] = true;
        } else if (b->command != CMD_RESTORE_BACKGROUND && b->command != CMD_SAFE_QUIT) {
            set_reason(reason, reason_len, "unknown command binding");
            return false;
        }

        if (b->command == CMD_RESTORE_BACKGROUND) {
            restore_count++;
            if (b->modifiers != MOD_NONE ||
                (b->key != KEY_R && b->key != KEY_B && b->key != KEY_BACKSPACE)) {
                set_reason(reason, reason_len, "background restore must use R, B or Backspace without modifiers");
                return false;
            }
        }
    }

    if (restore_count != 1) {
        set_reason(reason, reason_len, "background restore must have exactly one binding");
        return false;
    }

    return true;
}

static bool json_uint(const JsonValue *value, unsigned int *out) {
    if (value == NULL || value->type != JSON_NUMBER || value->u.number < 0) return false;
    *out = (unsigned int)value->u.number;
    return true;
}

static bool parse_policy_node(InputPolicy *policy, const JsonValue *node,
                              char *reason, size_t reason_len) {
    memset(policy, 0, sizeof(*policy));

    const JsonValue *bindings = json_object_get(node, "bindings");
    const JsonValue *repeat = json_object_get(node, "repeat");
    unsigned int generation = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    if (!json_uint(json_object_get(node, "generation"), &generation) ||
        bindings == NULL || bindings->type != JSON_ARRAY ||
        repeat == NULL || repeat->type != JSON_OBJECT ||
        !json_uint(json_object_get(repeat, "initial_ms"), &policy->repeat_initial_ms) ||
        !json_uint(json_object_get(repeat, "interval_ms"), &policy->repeat_interval_ms) ||
        !json_uint(json_object_get(repeat, "step_pixels"), (unsigned int *)&policy->step_pixels) ||
        !json_uint(json_object_get(node, "scene_width"), &width) ||
        !json_uint(json_object_get(node, "scene_height"), &height)) {
        set_reason(reason, reason_len, "policy fields missing or malformed");
        return false;
    }
    policy->generation = generation;
    policy->scene_width = (int)width;
    policy->scene_height = (int)height;

    if (bindings->u.array.count == 0 || bindings->u.array.count > INPUT_MAX_BINDINGS) {
        set_reason(reason, reason_len, "binding array size invalid");
        return false;
    }

    for (size_t i = 0; i < bindings->u.array.count; ++i) {
        const JsonValue *item = bindings->u.array.items[i];
        const JsonValue *key_node = json_object_get(item, "key");
        const JsonValue *command_node = json_object_get(item, "command");
        const JsonValue *modifiers_node = json_object_get(item, "modifiers");
        unsigned int modifiers = 0;
        CanonicalKey key;
        CommandId command;

        if (key_node == NULL || key_node->type != JSON_STRING ||
            command_node == NULL || command_node->type != JSON_STRING ||
            !canonical_key_from_name(key_node->u.string, &key) ||
            !command_from_name(command_node->u.string, &command) ||
            (modifiers_node != NULL && !json_uint(modifiers_node, &modifiers))) {
            set_reason(reason, reason_len, "binding is malformed");
            return false;
        }

        bool alias = false;
        const JsonValue *alias_node = json_object_get(item, "alias");
        if (alias_node != NULL && alias_node->type == JSON_BOOL) {
            alias = alias_node->u.boolean;
        }
        add_binding(policy, key, (uint16_t)modifiers, command, alias);
    }

    return input_policy_validate(policy, reason, reason_len);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool checksum_matches(const char *expected, const unsigned char *payload, size_t len) {
    char actual[SHA256_HEX_SIZE];
    sha256_hex(payload, len, actual);
    if (expected == NULL || strlen(expected) != 64) return false;
    for (int i = 0; i < 64; ++i) {
        if (hex_value(expected[i]) < 0 || expected[i] != actual[i]) return false;
    }
    return true;
}

PolicyParseResult policy_package_parse(
    const char *json,
    size_t json_len,
    PolicyPackage *package,
    InputPolicy *policy,
    char *reason,
    size_t reason_len
) {
    if (json == NULL || package == NULL || policy == NULL || json_len == 0 || json_len > INPUT_POLICY_JSON_CAP) {
        set_reason(reason, reason_len, "empty or oversized package");
        return POLICY_PARSE_INVALID_JSON;
    }
    memset(package, 0, sizeof(*package));

    JsonValue *root = json_parse(json, json_len);
    if (root == NULL || root->type != JSON_OBJECT) {
        json_free(root);
        set_reason(reason, reason_len, "package is not a JSON object");
        return POLICY_PARSE_INVALID_JSON;
    }

    unsigned int version = 0;
    unsigned int generation = 0;
    const JsonValue *version_node = json_object_get(root, "version");
    const JsonValue *generation_node = json_object_get(root, "generation");
    const JsonValue *payload_node = json_object_get(root, "payload");
    const JsonValue *checksum_node = json_object_get(root, "sha256");

    if (!json_uint(version_node, &version) || version != POLICY_VERSION ||
        !json_uint(generation_node, &generation) || generation == 0 ||
        payload_node == NULL || payload_node->type != JSON_STRING) {
        json_free(root);
        set_reason(reason, reason_len, "envelope version, generation or payload invalid");
        return POLICY_PARSE_INVALID_JSON;
    }

    size_t payload_len = strlen(payload_node->u.string);
    unsigned char *payload_copy = malloc(payload_len + 1);
    if (payload_copy == NULL) {
        json_free(root);
        set_reason(reason, reason_len, "out of memory");
        return POLICY_PARSE_INVALID_JSON;
    }
    memcpy(payload_copy, payload_node->u.string, payload_len + 1);

    if (checksum_node != NULL) {
        if (checksum_node->type != JSON_STRING ||
            !checksum_matches(checksum_node->u.string, payload_copy, payload_len)) {
            free(payload_copy);
            json_free(root);
            set_reason(reason, reason_len, "policy checksum mismatch");
            return POLICY_PARSE_CHECKSUM_MISMATCH;
        }
        snprintf(package->checksum, sizeof(package->checksum), "%s", checksum_node->u.string);
        package->checksum_present = true;
    }

    JsonValue *policy_root = json_parse((char *)payload_copy, payload_len);
    if (policy_root == NULL) {
        free(payload_copy);
        json_free(root);
        set_reason(reason, reason_len, "payload is not valid JSON");
        return POLICY_PARSE_INVALID_PAYLOAD;
    }

    InputPolicy parsed;
    if (!parse_policy_node(&parsed, policy_root, reason, reason_len)) {
        json_free(policy_root);
        free(payload_copy);
        json_free(root);
        return POLICY_PARSE_INVALID_PAYLOAD;
    }
    json_free(policy_root);
    json_free(root);

    if (parsed.generation != generation) {
        free(payload_copy);
        set_reason(reason, reason_len, "envelope and payload generation differ");
        return POLICY_PARSE_INVALID_PAYLOAD;
    }

    package->generation = generation;
    package->payload = payload_copy;
    package->payload_len = payload_len;
    *policy = parsed;
    return POLICY_PARSE_OK;
}

void policy_package_free(PolicyPackage *package) {
    if (package == NULL) return;
    free(package->payload);
    package->payload = NULL;
    package->payload_len = 0;
}

const char *policy_parse_result_name(PolicyParseResult result) {
    switch (result) {
        case POLICY_PARSE_OK: return "ok";
        case POLICY_PARSE_INVALID_JSON: return "invalid-json";
        case POLICY_PARSE_CHECKSUM_MISMATCH: return "checksum-mismatch";
        case POLICY_PARSE_INVALID_PAYLOAD: return "invalid-payload";
        default: return "unknown";
    }
}

void input_event_init(InputEvent *event, InputEventType type, uint32_t timestamp_ms) {
    if (event == NULL) return;
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->timestamp_ms = timestamp_ms;
    event->key = KEY_NONE;
    event->focused = true;
    event->display_present = true;
    event->layer = LAYER_PRESENTATION;
}

static ActivePress *find_press(InputCore *core, CanonicalKey key) {
    for (int i = 0; i < INPUT_MAX_PRESSES; ++i) {
        if (core->presses[i].held && core->presses[i].key == key) {
            return &core->presses[i];
        }
    }
    return NULL;
}

static ActivePress *free_press_slot(InputCore *core) {
    for (int i = 0; i < INPUT_MAX_PRESSES; ++i) {
        if (!core->presses[i].held) return &core->presses[i];
    }
    return NULL;
}

static ActivePress *find_move_press(InputCore *core) {
    for (int i = 0; i < INPUT_MAX_PRESSES; ++i) {
        ActivePress *p = &core->presses[i];
        if (!p->held) continue;
        for (int j = 0; j < core->active.binding_count; ++j) {
            KeyBinding *b = &core->active.bindings[j];
            if (b->key == p->key && is_move_command(b->command)) return p;
        }
    }
    return NULL;
}

static const KeyBinding *find_binding(const InputPolicy *policy, CanonicalKey key, uint16_t modifiers) {
    for (int i = 0; i < policy->binding_count; ++i) {
        if (policy->bindings[i].key == key && policy->bindings[i].modifiers == modifiers) {
            return &policy->bindings[i];
        }
    }
    return NULL;
}

static void out_add(OutputList *out, OutputEvent event) {
    if (out == NULL) return;
    if (out->count < (int)(sizeof(out->items) / sizeof(out->items[0]))) {
        out->items[out->count++] = event;
    }
}

static void out_ignore(OutputList *out) {
    if (out != NULL && out->count == 0) {
        OutputEvent e = {0};
        e.type = OUT_IGNORE;
        out_add(out, e);
    }
}

static void out_policy(OutputList *out, uint32_t generation, PolicyDeliveryStatus status,
                       const char *reason) {
    OutputEvent e = {0};
    e.type = OUT_POLICY_STATUS;
    e.generation = generation;
    e.status = status;
    if (reason != NULL) snprintf(e.reason, sizeof(e.reason), "%s", reason);
    out_add(out, e);
}

static int clamp_i(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static int default_x(const InputPolicy *policy) {
    return (policy->scene_width - MARKER_SIZE) / 2;
}

static int default_y(const InputPolicy *policy) {
    return (policy->scene_height - MARKER_SIZE) / 2;
}

static void clamp_marker(InputCore *core) {
    core->marker_x = clamp_i(core->marker_x, 0, core->active.scene_width - MARKER_SIZE);
    core->marker_y = clamp_i(core->marker_y, 0, core->active.scene_height - MARKER_SIZE);
}

void input_core_init(InputCore *core, int width, int height, TrustMode trust) {
    if (core == NULL) return;
    memset(core, 0, sizeof(*core));
    input_policy_default(&core->active, width, height);
    core->applied_generation = 0;
    core->marker_x = default_x(&core->active);
    core->marker_y = default_y(&core->active);
    core->focused = true;
    core->display_present = true;
    core->layer = LAYER_PRESENTATION;
    core->trust = trust;
    core->repeating_key = KEY_NONE;
}

void input_core_resize(InputCore *core, int width, int height) {
    if (core == NULL || width <= MARKER_SIZE || height <= MARKER_SIZE) return;
    core->active.scene_width = width;
    core->active.scene_height = height;
    if (core->has_staged) {
        core->staged.scene_width = width;
        core->staged.scene_height = height;
    }
    clamp_marker(core);
}

static void emit_move(InputCore *core, OutputList *out, CommandId command) {
    int dx = 0;
    int dy = 0;
    int step = core->active.step_pixels;
    switch (command) {
        case CMD_MOVE_LEFT: dx = -step; break;
        case CMD_MOVE_RIGHT: dx = step; break;
        case CMD_MOVE_UP: dy = -step; break;
        case CMD_MOVE_DOWN: dy = step; break;
        default: return;
    }
    int old_x = core->marker_x;
    int old_y = core->marker_y;
    core->marker_x = clamp_i(old_x + dx, 0, core->active.scene_width - MARKER_SIZE);
    core->marker_y = clamp_i(old_y + dy, 0, core->active.scene_height - MARKER_SIZE);
    if (core->marker_x != old_x || core->marker_y != old_y) {
        OutputEvent e = {0};
        e.type = OUT_MOVE;
        e.command = command;
        e.dx = core->marker_x - old_x;
        e.dy = core->marker_y - old_y;
        e.x = core->marker_x;
        e.y = core->marker_y;
        out_add(out, e);
    }
}

static bool apply_staged_if_idle(InputCore *core, OutputList *out) {
    if (!core->has_staged) return false;
    for (int i = 0; i < INPUT_MAX_PRESSES; ++i) {
        if (core->presses[i].held) return false;
    }

    uint32_t generation = core->staged.generation;
    int old_w = core->active.scene_width;
    int old_h = core->active.scene_height;
    core->active = core->staged;
    core->active.scene_width = old_w;
    core->active.scene_height = old_h;
    core->staged = (InputPolicy){0};
    core->has_staged = false;
    core->applied_generation = generation;
    core->repeating_key = KEY_NONE;
    clamp_marker(core);
    out_policy(out, generation, POLICY_STATUS_ACTIVATED, "quiescent boundary");
    return true;
}

void input_core_reset_presses(InputCore *core, const char *reason, OutputList *out) {
    (void)reason;
    if (core == NULL) return;
    memset(core->presses, 0, sizeof(core->presses));
    core->repeating_key = KEY_NONE;
    apply_staged_if_idle(core, out);
}

bool input_core_stage_policy(InputCore *core, const InputPolicy *wanted, uint32_t generation,
                             const char *checksum, OutputList *out) {
    (void)checksum;
    char reason[INPUT_REASON_CAP];
    if (core == NULL || wanted == NULL) {
        out_policy(out, generation, POLICY_STATUS_REJECTED, "null policy");
        return false;
    }
    if (generation <= core->applied_generation ||
        (core->has_staged && generation <= core->staged.generation)) {
        out_policy(out, generation, POLICY_STATUS_REJECTED, "stale generation");
        return false;
    }
    if (!input_policy_validate(wanted, reason, sizeof(reason))) {
        out_policy(out, generation, POLICY_STATUS_REJECTED, reason);
        return false;
    }

    core->staged = *wanted;
    core->staged.generation = generation;
    core->staged.scene_width = core->active.scene_width;
    core->staged.scene_height = core->active.scene_height;
    core->has_staged = true;

    bool applied = apply_staged_if_idle(core, out);
    if (!applied) {
        out_policy(out, generation, POLICY_STATUS_STAGED,
                   "waiting for old physical keys to reach the up boundary");
    }
    return true;
}

static bool is_safe_quit(const InputEvent *event) {
    return event->key == KEY_Q && event->phase == PHASE_DOWN &&
           (event->modifiers & MOD_SAFE_QUIT) == MOD_SAFE_QUIT;
}

static void start_move(InputCore *core, const InputEvent *event,
                       const KeyBinding *binding, OutputList *out) {
    ActivePress *existing = find_press(core, event->key);
    if (existing != NULL) return;
    ActivePress *slot = free_press_slot(core);
    if (slot == NULL) return;
    slot->held = true;
    slot->key = event->key;
    slot->generation = core->active.generation;
    slot->scancode = event->scancode;

    core->repeating_key = event->key;
    core->repeat_start_ms = event->timestamp_ms;
    core->last_repeat_ms = event->timestamp_ms;
    emit_move(core, out, binding->command);
}

static CommandId command_for_press(InputCore *core, CanonicalKey key) {
    for (int i = 0; i < core->active.binding_count; ++i) {
        if (core->active.bindings[i].key == key) {
            return core->active.bindings[i].command;
        }
    }
    return CMD_NONE;
}

static void handle_key(InputCore *core, const InputEvent *event, OutputList *out) {
    if (!event->focused || !event->display_present) {
        out_ignore(out);
        return;
    }

    if (is_safe_quit(event)) {
        OutputEvent e = {0};
        e.type = OUT_SAFE_QUIT;
        e.command = CMD_SAFE_QUIT;
        e.key = KEY_Q;
        out_add(out, e);
        return;
    }

    if (event->phase == PHASE_UP) {
        ActivePress *press = find_press(core, event->key);
        if (press != NULL) {
            memset(press, 0, sizeof(*press));
        }
        if (core->repeating_key == event->key) {
            ActivePress *another = find_move_press(core);
            core->repeating_key = another == NULL ? KEY_NONE : another->key;
            core->last_repeat_ms = event->timestamp_ms;
        }
        apply_staged_if_idle(core, out);
        return;
    }

    if (event->composing) {
        /* The IME owns the whole physical chord during composition. */
        out_ignore(out);
        return;
    }

    if (core->layer == LAYER_MODAL) {
        if (event->phase == PHASE_DOWN && event->key == KEY_ESCAPE &&
            event->modifiers == MOD_NONE) {
            OutputEvent e = {0};
            e.type = OUT_CANCEL_MODAL;
            e.command = CMD_CANCEL_MODAL;
            e.key = KEY_ESCAPE;
            out_add(out, e);
            return;
        }
        out_ignore(out);
        return;
    }

    if (core->layer == LAYER_TEXTBOX) {
        if (event->phase == PHASE_DOWN && event->key == KEY_ESCAPE &&
            event->modifiers == MOD_NONE) {
            OutputEvent e = {0};
            e.type = OUT_CANCEL_MODAL;
            e.command = CMD_CANCEL_MODAL;
            e.key = KEY_ESCAPE;
            out_add(out, e);
        } else {
            out_ignore(out);
        }
        return;
    }

    const KeyBinding *binding = find_binding(&core->active, event->key, event->modifiers);
    if (binding == NULL) {
        out_ignore(out);
        return;
    }

    if (is_move_command(binding->command)) {
        if (event->phase == PHASE_REPEAT) {
            out_ignore(out);
            return;
        }
        start_move(core, event, binding, out);
        return;
    }

    if (event->phase != PHASE_DOWN || find_press(core, event->key) != NULL) {
        out_ignore(out);
        return;
    }

    if (find_press(core, event->key) != NULL) {
        out_ignore(out);
        return;
    }
    ActivePress *slot = free_press_slot(core);
    if (slot != NULL) {
        slot->held = true;
        slot->key = event->key;
        slot->generation = core->active.generation;
        slot->scancode = event->scancode;
    }

    switch (binding->command) {
        case CMD_TOGGLE_FULLSCREEN:
            if (core->fullscreen_switching) {
                out_ignore(out);
                break;
            }
            core->fullscreen_switching = true;
            core->fullscreen_since_ms = event->timestamp_ms;
            {
                OutputEvent e = {0};
                e.type = OUT_FULLSCREEN_TOGGLE;
                e.command = CMD_TOGGLE_FULLSCREEN;
                e.key = KEY_F11;
                e.target_fullscreen = !core->fullscreen;
                out_add(out, e);
            }
            break;
        case CMD_RESTORE_BACKGROUND:
            core->marker_x = default_x(&core->active);
            core->marker_y = default_y(&core->active);
            {
                OutputEvent e = {0};
                e.type = OUT_RESTORE_BACKGROUND;
                e.command = CMD_RESTORE_BACKGROUND;
                e.key = binding->key;
                e.x = core->marker_x;
                e.y = core->marker_y;
                out_add(out, e);
            }
            break;
        case CMD_CANCEL_MODAL:
            out_ignore(out);
            break;
        default:
            out_ignore(out);
            break;
    }
}

static void handle_tick(InputCore *core, const InputEvent *event, OutputList *out) {
    if (core->fullscreen_switching &&
        event->timestamp_ms - core->fullscreen_since_ms > SAFE_QUIT_TIMEOUT_MS) {
        /* Platform confirmation is lost; allow a later explicit user edge without oscillation. */
        core->fullscreen_switching = false;
    }

    if (!core->focused || !core->display_present || core->composing ||
        core->layer != LAYER_PRESENTATION || core->repeating_key == KEY_NONE) {
        return;
    }

    ActivePress *press = find_press(core, core->repeating_key);
    if (press == NULL) {
        core->repeating_key = KEY_NONE;
        return;
    }

    CommandId command = command_for_press(core, core->repeating_key);
    if (!is_move_command(command)) {
        core->repeating_key = KEY_NONE;
        return;
    }

    uint32_t held = event->timestamp_ms - core->repeat_start_ms;
    if (held < core->active.repeat_initial_ms) return;
    if (event->timestamp_ms - core->last_repeat_ms < core->active.repeat_interval_ms) return;

    core->last_repeat_ms = event->timestamp_ms;
    emit_move(core, out, command);
}

void input_core_process(InputCore *core, const InputEvent *event, OutputList *out) {
    if (core == NULL || event == NULL || out == NULL) return;
    memset(out, 0, sizeof(*out));

    switch (event->type) {
        case EV_KEY:
            handle_key(core, event, out);
            break;
        case EV_FOCUS:
            core->focused = event->focused;
            if (!event->focused) {
                input_core_reset_presses(core, "window blur", out);
            }
            break;
        case EV_COMPOSITION:
            core->composing = event->composing;
            break;
        case EV_LAYER:
            core->layer = event->layer;
            break;
        case EV_FULLSCREEN_STATE:
            core->fullscreen = event->fullscreen;
            core->fullscreen_switching = false;
            break;
        case EV_DISPLAY:
            core->display_present = event->display_present;
            if (!event->display_present) {
                bool was_fullscreen = core->fullscreen;
                memset(core->presses, 0, sizeof(core->presses));
                core->repeating_key = KEY_NONE;
                core->fullscreen = false;
                core->fullscreen_switching = false;
                if (was_fullscreen) {
                    OutputEvent e = {0};
                    e.type = OUT_FULLSCREEN_TOGGLE;
                    e.target_fullscreen = false;
                    out_add(out, e);
                }
                apply_staged_if_idle(core, out);
            }
            break;
        case EV_TICK:
            handle_tick(core, event, out);
            break;
        case EV_POLICY_LOADED:
            break;
        case EV_POLICY_STAGE:
            apply_staged_if_idle(core, out);
            break;
        case EV_POLICY_REJECTED:
            core->last_generation = event->generation;
            snprintf(core->last_reject_reason, sizeof(core->last_reject_reason),
                     "%s", event->text);
            out_policy(out, event->generation, POLICY_STATUS_REJECTED, event->text);
            break;
    }
}
