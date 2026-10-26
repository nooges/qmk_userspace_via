// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Boards without RGB matrix (e.g. an RGB light revision sharing the VIA keymap)
// build without the module's storage and sync
#ifdef RGB_MATRIX_ENABLE

// Lighting layers: layer 0 is the base paint, higher layers overlay it while active
#ifndef VIA_PER_KEY_LAYERS
#    define VIA_PER_KEY_LAYERS 4
#endif

// Per lighting layer and per indicator overlay (Caps/Num/Scroll Lock, Caps Word):
// hue/sat and brightness per LED, plus a transparent bit per LED. Then the
// RGB sleep timer in ms (uint32).
// Bump the version if the layout of this block changes.
#define EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_SIZE ((VIA_PER_KEY_LAYERS + 4) * (RGB_MATRIX_LED_COUNT * 3 + (RGB_MATRIX_LED_COUNT + 7) / 8) + 4)
#define EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_VERSION 5

// The slave half renders layer overlays too, so it needs the active layers
#ifndef SPLIT_LAYER_STATE_ENABLE
#    define SPLIT_LAYER_STATE_ENABLE
#endif

// The RGB sleep timer can be turned on from VIA, and the slave half needs the
// master's key activity to time out with it
#ifndef SPLIT_ACTIVITY_ENABLE
#    define SPLIT_ACTIVITY_ENABLE
#endif

// Split RPC used to mirror the per-key buffer to the slave half
#define SPLIT_TRANSACTION_IDS_MODULE_VIA_PER_KEY_RGB PUT_VIA_PER_KEY_RGB

// Reported to VIA as id_firmware_version. The VIA definition only shows (and reads)
// controls the firmware supports, so it still works on older firmware:
//   1 = per-key colors, 2 = lighting layers, indicator overlays and RGB sleep timer
#ifndef VIA_FIRMWARE_VERSION
#    define VIA_FIRMWARE_VERSION 2
#endif

#endif // RGB_MATRIX_ENABLE
