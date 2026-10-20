// SPDX-License-Identifier: GPL-2.0-or-later
//
// VIA per-key RGB ("Direct" effect)
//
// VIA's key painter (color-palette control) sends, on custom channel 0:
//   id_custom_set_value  [0x07, 0, 1, led_index, count, hue, sat, ...]
//   id_custom_get_value  [0x08, 0, 1, led_index, count]  -> hue/sat at data[5..]
//   id_custom_save       [0x09, 0]   (sent after EVERY painted key)
//
// Underglow LEDs have no key to paint, so a plain color control sets them as a group:
//   id_custom_set_value  [0x07, 0, 2, hue, sat]  -> written to every LED_FLAG_UNDERGLOW LED
//   id_custom_get_value  [0x08, 0, 2]            -> hue/sat of the first underglow LED
//
// Per-LED brightness (0 = off), scaled by the global RGB matrix V. Setting hue/sat
// (value IDs 1 and 2) resets it to full, so painting with stock VIA turns LEDs back on:
//   id_custom_set_value  [0x07, 0, 3, led_index, count, val, ...]
//   id_custom_get_value  [0x08, 0, 3, led_index, count]  -> val at data[5..]
//
// Hue/sat per LED live in pkr_hs[], brightness in pkr_v[].
// Saves are debounced to spare flash, and on split boards the buffer is pushed
// to the slave half over a split RPC, since VIA only talks to the master.
//
// VIA_PER_KEY_EFFECT_ID must equal the "Direct" option value in the board's VIA
// definition. Module effects sort right after the core effects, so it is 1 + the
// number of enabled core effects: 45 for boards that enable exactly the effects in
// VIA's built-in qmk_rgb_matrix menu. Override it in the keymap's config.h otherwise.

#include QMK_KEYBOARD_H
#include <string.h>
#include "via.h"
#include "community_modules.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 1, 3);

#ifdef SPLIT_KEYBOARD
#    include "transactions.h"
#    include "split_util.h"
#endif

#ifndef VIA_PER_KEY_SAVE_DELAY_MS
#    define VIA_PER_KEY_SAVE_DELAY_MS 1500
#endif

#ifndef VIA_PER_KEY_EFFECT_ID
#    define VIA_PER_KEY_EFFECT_ID 45
#endif

#define PKR_VALUE_ID 1
#define PKR_UNDERGLOW_VALUE_ID 2
#define PKR_BRIGHTNESS_VALUE_ID 3

// Read by the effect in rgb_matrix_module.inc
uint8_t pkr_hs[RGB_MATRIX_LED_COUNT][2];
uint8_t pkr_v[RGB_MATRIX_LED_COUNT];

_Static_assert(RGB_MATRIX_LED_COUNT <= 255, "VIA per-key protocol uses an 8-bit LED index");
_Static_assert(EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_SIZE >= sizeof(pkr_hs) + sizeof(pkr_v), "EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_SIZE too small");
_Static_assert(RGB_MATRIX_COMMUNITY_MODULE_PER_KEY_VIA == VIA_PER_KEY_EFFECT_ID,
               "Direct effect ID differs from VIA_PER_KEY_EFFECT_ID; set it in the keymap's config.h and use the same --direct-id for the VIA definition");

static bool     pkr_save_pending = false;
static uint32_t pkr_save_timer   = 0;

#ifdef SPLIT_KEYBOARD
// [start, count, h0, s0, v0, h1, s1, v1, ...] must fit in one RPC packet
#    define PKR_CHUNK_LEDS ((RPC_M2S_BUFFER_SIZE - 2) / 3)
#    define PKR_CHUNKS ((RGB_MATRIX_LED_COUNT + PKR_CHUNK_LEDS - 1) / PKR_CHUNK_LEDS)
_Static_assert(PKR_CHUNKS <= 32, "Too many sync chunks for the dirty mask; raise RPC_M2S_BUFFER_SIZE");

static uint32_t pkr_dirty_chunks = (PKR_CHUNKS >= 32) ? 0xFFFFFFFFu : ((1u << PKR_CHUNKS) - 1); // push everything once at boot
static uint8_t  pkr_refresh_chunk = 0;
static uint8_t  pkr_idle_ticks    = 0;
static uint32_t pkr_sync_timer    = 0;

static void pkr_mark_dirty(uint8_t start, uint8_t count) {
    for (uint8_t i = start; i < start + count; i++) {
        pkr_dirty_chunks |= 1u << (i / PKR_CHUNK_LEDS);
    }
}

static bool pkr_send_chunk(uint8_t chunk) {
    uint8_t buf[RPC_M2S_BUFFER_SIZE];
    uint8_t start = chunk * PKR_CHUNK_LEDS;
    uint8_t count = MIN(PKR_CHUNK_LEDS, RGB_MATRIX_LED_COUNT - start);
    buf[0]        = start;
    buf[1]        = count;
    for (uint8_t i = 0; i < count; i++) {
        buf[2 + i * 3]     = pkr_hs[start + i][0];
        buf[2 + i * 3 + 1] = pkr_hs[start + i][1];
        buf[2 + i * 3 + 2] = pkr_v[start + i];
    }
    return transaction_rpc_send(PUT_VIA_PER_KEY_RGB, 2 + count * 3, buf);
}

static void pkr_slave_handler(uint8_t in_len, const void *in_data, uint8_t out_len, void *out_data) {
    const uint8_t *d = (const uint8_t *)in_data;
    if (in_len < 2) return;
    uint8_t start = d[0], count = d[1];
    if (start + count > RGB_MATRIX_LED_COUNT || in_len < 2 + count * 3) return;
    for (uint8_t i = 0; i < count; i++) {
        pkr_hs[start + i][0] = d[2 + i * 3];
        pkr_hs[start + i][1] = d[2 + i * 3 + 1];
        pkr_v[start + i]     = d[2 + i * 3 + 2];
    }
}
#else
static inline void pkr_mark_dirty(uint8_t start, uint8_t count) {}
#endif

static void pkr_read(void) {
    eeconfig_read_via_per_key_rgb_datablock(pkr_hs, 0, sizeof(pkr_hs));
    eeconfig_read_via_per_key_rgb_datablock(pkr_v, sizeof(pkr_hs), sizeof(pkr_v));
}

static void pkr_write(void) {
    eeconfig_update_via_per_key_rgb_datablock(pkr_hs, 0, sizeof(pkr_hs));
    eeconfig_update_via_per_key_rgb_datablock(pkr_v, sizeof(pkr_hs), sizeof(pkr_v));
}

// Fresh/cleared EEPROM: start every LED at red rather than the zeroed block's white
void eeconfig_init_via_per_key_rgb_datablock(void) {
    for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
        pkr_hs[i][0] = 0;   // hue: red
        pkr_hs[i][1] = 255; // sat
        pkr_v[i]     = 255;
    }
    pkr_mark_dirty(0, RGB_MATRIX_LED_COUNT);
    pkr_write();
}

void keyboard_post_init_via_per_key_rgb(void) {
    pkr_read();
#ifdef SPLIT_KEYBOARD
    transaction_register_rpc(PUT_VIA_PER_KEY_RGB, pkr_slave_handler);
#endif
}

void housekeeping_task_via_per_key_rgb(void) {
    if (pkr_save_pending && timer_elapsed32(pkr_save_timer) > VIA_PER_KEY_SAVE_DELAY_MS) {
        pkr_save_pending = false;
        pkr_write();
    }

#ifdef SPLIT_KEYBOARD
    if (!is_keyboard_master() || !is_transport_connected()) return;
    if (timer_elapsed32(pkr_sync_timer) < 10) return;
    pkr_sync_timer = timer_read32();

    if (pkr_dirty_chunks) {
        uint8_t chunk = __builtin_ctz(pkr_dirty_chunks);
        if (pkr_send_chunk(chunk)) pkr_dirty_chunks &= ~(1u << chunk);
    } else if (++pkr_idle_ticks >= 25) {
        // Slow round-robin refresh (~1 chunk / 250 ms) covers a slave half that
        // was plugged in or reset after boot.
        pkr_idle_ticks = 0;
        pkr_send_chunk(pkr_refresh_chunk);
        pkr_refresh_chunk = (pkr_refresh_chunk + 1) % PKR_CHUNKS;
    }
#endif
}

void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    uint8_t *command_id = &data[0];
    uint8_t  channel_id = data[1];

    if (channel_id != id_custom_channel) {
        *command_id = id_unhandled;
        return;
    }

    if (*command_id == id_custom_save) {
        pkr_save_pending = true;
        pkr_save_timer   = timer_read32();
        return;
    }

    uint8_t value_id = data[2];

    if (value_id == PKR_UNDERGLOW_VALUE_ID) {
        if (length < 5) {
            *command_id = id_unhandled;
            return;
        }
        bool first = true;
        for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
            if (!HAS_FLAGS(g_led_config.flags[i], LED_FLAG_UNDERGLOW)) continue;
            if (*command_id == id_custom_set_value) {
                pkr_hs[i][0] = data[3];
                pkr_hs[i][1] = data[4];
                pkr_v[i]     = 255;
                pkr_mark_dirty(i, 1);
            } else if (*command_id == id_custom_get_value && first) {
                data[3] = pkr_hs[i][0];
                data[4] = pkr_hs[i][1];
            }
            first = false;
        }
        if (*command_id != id_custom_set_value && *command_id != id_custom_get_value) {
            *command_id = id_unhandled;
        }
        return;
    }

    uint8_t start = data[3];
    uint8_t count = data[4];

    if (value_id == PKR_BRIGHTNESS_VALUE_ID) {
        if (start + count > RGB_MATRIX_LED_COUNT || 5 + count > length) {
            *command_id = id_unhandled;
            return;
        }
        switch (*command_id) {
            case id_custom_set_value:
                memcpy(&pkr_v[start], &data[5], count);
                pkr_mark_dirty(start, count);
                break;
            case id_custom_get_value:
                memcpy(&data[5], &pkr_v[start], count);
                break;
            default:
                *command_id = id_unhandled;
                break;
        }
        return;
    }

    if (value_id != PKR_VALUE_ID || start + count > RGB_MATRIX_LED_COUNT || 5 + count * 2 > length) {
        *command_id = id_unhandled;
        return;
    }

    switch (*command_id) {
        case id_custom_set_value:
            memcpy(&pkr_hs[start], &data[5], count * 2);
            memset(&pkr_v[start], 255, count);
            pkr_mark_dirty(start, count);
            break;
        case id_custom_get_value:
            memcpy(&data[5], &pkr_hs[start], count * 2);
            break;
        default:
            *command_id = id_unhandled;
            break;
    }
}
