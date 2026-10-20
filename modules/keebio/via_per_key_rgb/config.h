// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Per-LED hue/sat, then per-LED brightness. Bump the version if the layout of this block changes.
#define EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_SIZE (RGB_MATRIX_LED_COUNT * 3)
#define EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_VERSION 2

// Split RPC used to mirror the per-key buffer to the slave half
#define SPLIT_TRANSACTION_IDS_MODULE_VIA_PER_KEY_RGB PUT_VIA_PER_KEY_RGB

// Reported to VIA as id_firmware_version. The VIA definition only shows (and reads)
// the per-key controls at this version or later, so it still works on older firmware.
#ifndef VIA_FIRMWARE_VERSION
#    define VIA_FIRMWARE_VERSION 1
#endif
