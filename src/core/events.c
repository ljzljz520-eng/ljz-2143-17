#include "events.h"

#include <string.h>

const char *origin_name(EventOrigin o) {
    switch (o) {
    case ORIGIN_HUMAN:  return "human";
    case ORIGIN_REPLAY: return "replay";
    case ORIGIN_REMOTE: return "remote";
    }
    return "?";
}

InputEvent ie_key(EventOrigin origin, NK_Key key, uint32_t mods, bool down,
                  bool repeat, bool in_composition) {
    InputEvent e;
    memset(&e, 0, sizeof e);
    e.type = IE_KEY;
    e.origin = origin;
    e.key = key;
    e.mods = mods;
    e.down = down;
    e.repeat = repeat;
    e.composition = in_composition;
    return e;
}
