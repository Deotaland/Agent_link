// Between muse_gadget.c (the API) and the pieces of the port that report into it.
#pragma once

#include "muse_gadget.h"

#ifdef __cplusplus
extern "C" {
#endif

// From led_status_gadget.c, which stands in for Home Link's status light.
void muse_gadget_emit_state(muse_gadget_state_t state);
void muse_gadget_emit_title(const char* name);

#ifdef __cplusplus
}
#endif
