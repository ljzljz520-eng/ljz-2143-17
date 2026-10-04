#ifndef INPUT_POLICY_H
#define INPUT_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_MAX_BINDINGS 32
#define INPUT_MAX_PRESSES 16
#define INPUT_TEXT_CAP 128
#define INPUT_REASON_CAP 160
#define INPUT_CHECKSUM_CAP SHA256_HEX_SIZE
#define INPUT_POLICY_JSON_CAP 8192

typedef enum {
    KEY_NONE = 0,
    KEY_ESCAPE,
    KEY_F11,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_R,
    KEY_B,
    KEY_BACKSPACE,
    KEY_Q,
    KEY_W,
    KEY_A,
    KEY_S,
    KEY_D,
    KEY_COUNT
} CanonicalKey;

typedef enum {
    CMD_NONE = 0,
    CMD_CANCEL_MODAL,
    CMD_TOGGLE_FULLSCREEN,
    CMD_MOVE_LEFT,
    CMD_MOVE_RIGHT,
    CMD_MOVE_UP,
    CMD_MOVE_DOWN,
    CMD_RESTORE_BACKGROUND,
    CMD_SAFE_QUIT,
    CMD_COUNT
} CommandId;

typedef enum {
    EV_KEY = 1,
    EV_FOCUS,
    EV_COMPOSITION,
    EV_LAYER,
    EV_FULLSCREEN_STATE,
    EV_DISPLAY,
    EV_TICK,
    EV_POLICY_STAGE,
    EV_POLICY_LOADED,
    EV_POLICY_REJECTED
} InputEventType;

typedef enum { PHASE_UP = 0, PHASE_DOWN = 1, PHASE_REPEAT = 2 } KeyPhase;
typedef enum { LAYER_PRESENTATION = 0, LAYER_TEXTBOX = 1, LAYER_MODAL = 2 } InputLayer;
typedef enum { TRUST_OPERATIONAL = 0, TRUST_EXPERIMENT = 1 } TrustMode;
typedef enum {
    POLICY_STATUS_STAGED = 1,
    POLICY_STATUS_ACTIVATED = 2,
    POLICY_STATUS_REJECTED = 3
} PolicyDeliveryStatus;

enum {
    MOD_NONE = 0,
    MOD_LSHIFT = 1u << 0,
    MOD_RSHIFT = 1u << 1,
    MOD_LCTRL = 1u << 2,
    MOD_RCTRL = 1u << 3,
    MOD_LALT = 1u << 4,
    MOD_RALT = 1u << 5,
    MOD_LGUI = 1u << 6,
    MOD_RGUI = 1u << 7,
    MOD_SHIFT = MOD_LSHIFT | MOD_RSHIFT,
    MOD_CTRL = MOD_LCTRL | MOD_RCTRL,
    MOD_ALT = MOD_LALT | MOD_RALT,
    MOD_GUI = MOD_LGUI | MOD_RGUI,
    MOD_SAFE_QUIT = MOD_CTRL | MOD_SHIFT
};

typedef struct {
    CanonicalKey key;
    uint16_t modifiers;
    CommandId command;
    bool protected_key;
    bool alias;
} KeyBinding;

typedef struct {
    uint32_t generation;
    int binding_count;
    KeyBinding bindings[INPUT_MAX_BINDINGS];
    uint32_t repeat_initial_ms;
    uint32_t repeat_interval_ms;
    int step_pixels;
    int scene_width;
    int scene_height;
} InputPolicy;

typedef struct {
    CanonicalKey key;
    uint32_t generation;
    uint32_t scancode;
    bool held;
    int x;
    int y;
} ActivePress;

typedef struct {
    InputEventType type;
    uint32_t timestamp_ms;
    KeyPhase phase;
    CanonicalKey key;
    uint16_t modifiers;
    uint32_t scancode;
    bool focused;
    bool composing;
    InputLayer layer;
    bool fullscreen;
    bool display_present;
    uint32_t generation;
    char text[INPUT_TEXT_CAP];
} InputEvent;

typedef enum {
    OUT_IGNORE = 0,
    OUT_CANCEL_MODAL,
    OUT_FULLSCREEN_TOGGLE,
    OUT_MOVE,
    OUT_RESTORE_BACKGROUND,
    OUT_SAFE_QUIT,
    OUT_POLICY_STATUS
} OutputType;

typedef struct {
    OutputType type;
    CanonicalKey key;
    CommandId command;
    int dx;
    int dy;
    int x;
    int y;
    bool target_fullscreen;
    uint32_t generation;
    PolicyDeliveryStatus status;
    char reason[INPUT_REASON_CAP];
} OutputEvent;

typedef struct {
    OutputEvent items[8];
    int count;
} OutputList;

typedef struct {
    InputPolicy active;
    InputPolicy staged;
    bool has_staged;
    uint32_t applied_generation;
    ActivePress presses[INPUT_MAX_PRESSES];
    CanonicalKey repeating_key;
    uint32_t repeat_start_ms;
    uint32_t last_repeat_ms;
    int marker_x;
    int marker_y;
    InputLayer layer;
    bool focused;
    bool composing;
    bool fullscreen;
    bool fullscreen_switching;
    bool display_present;
    uint32_t fullscreen_since_ms;
    TrustMode trust;
    uint32_t last_generation;
    char last_reject_reason[INPUT_REASON_CAP];
} InputCore;

typedef struct {
    uint32_t generation;
    char checksum[INPUT_CHECKSUM_CAP];
    unsigned char *payload;
    size_t payload_len;
    bool checksum_present;
} PolicyPackage;

typedef enum {
    POLICY_PARSE_OK = 0,
    POLICY_PARSE_INVALID_JSON,
    POLICY_PARSE_CHECKSUM_MISMATCH,
    POLICY_PARSE_INVALID_PAYLOAD
} PolicyParseResult;

void input_policy_default(InputPolicy *policy, int width, int height);
bool input_policy_validate(const InputPolicy *policy, char *reason, size_t reason_len);
const char *canonical_key_name(CanonicalKey key);
const char *command_name(CommandId command);
bool canonical_key_from_name(const char *name, CanonicalKey *key);
bool command_from_name(const char *name, CommandId *command);

void input_core_init(InputCore *core, int width, int height, TrustMode trust);
void input_core_resize(InputCore *core, int width, int height);
void input_core_reset_presses(InputCore *core, const char *reason, OutputList *out);
void input_core_process(InputCore *core, const InputEvent *event, OutputList *out);

PolicyParseResult policy_package_parse(
    const char *json,
    size_t json_len,
    PolicyPackage *package,
    InputPolicy *policy,
    char *reason,
    size_t reason_len
);
void policy_package_free(PolicyPackage *package);
bool input_core_stage_policy(
    InputCore *core,
    const InputPolicy *wanted,
    uint32_t generation,
    const char *checksum,
    OutputList *out
);

const char *policy_parse_result_name(PolicyParseResult result);
void input_event_init(InputEvent *event, InputEventType type, uint32_t timestamp_ms);

#ifdef __cplusplus
}
#endif

#endif
