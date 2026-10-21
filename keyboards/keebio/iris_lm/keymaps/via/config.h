// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The default 1 KB of emulated EEPROM can't hold the VIA keymap plus the per-key
// lighting layers and indicators (~1.7 KB). 6 KB of flash backing gives 3 KB of
// EEPROM; more doesn't fit in RAM, since wear leveling keeps a RAM copy of it all.
#define WEAR_LEVELING_BACKING_SIZE 6144
