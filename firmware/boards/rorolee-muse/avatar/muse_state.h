// The one piece of Muse's muse_state.h an avatar renderer (muse_pixel.c) needs: the modes it
// animates. Same names and values as facebookincubator/muse-gadget-sdk components/muse/muse_state.h
// (Apache-2.0, Copyright (c) Meta Platforms, Inc. and affiliates), so a renderer written for a
// Muse board compiles here unchanged.
#pragma once

#include <stdbool.h>

typedef enum {
    MUSE_MODE_BOOT = 0,
    MUSE_MODE_IDLE,
    MUSE_MODE_LISTENING,
    MUSE_MODE_THINKING,
    MUSE_MODE_SPEAKING,
    MUSE_MODE_ERROR,
    MUSE_MODE_OFF,       /* powering down */
    MUSE_MODE_COUNT,
} muse_mode_t;
