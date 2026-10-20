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
// Value IDs 1-3 act on layer 0, the base paint. Higher layers overlay it: an LED that
// is transparent on a layer shows the next active layer below, like KC_TRNS. The
// effect uses, per LED, the highest active layer where that LED isn't transparent.
//   id_custom_set_value  [0x07, 0, 4, layer, led_index, count, hue, sat, val, transparent, ...]
//   id_custom_get_value  [0x08, 0, 4, layer, led_index, count]  -> 4 bytes per LED at data[6..]
//   id_custom_get_value  [0x08, 0, 5]  -> number of lighting layers at data[3]
// Layer 0 ignores the transparent byte.
//
// Saves are debounced to spare flash, and on split boards the buffers are pushed
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
#define PKR_LAYERED_VALUE_ID 4
#define PKR_LAYER_COUNT_VALUE_ID 5

#define PKR_LAYERS VIA_PER_KEY_LAYERS
#define PKR_MASK_BYTES ((RGB_MATRIX_LED_COUNT + 7) / 8)

// Read by the effect in rgb_matrix_module.inc
uint8_t        pkr_hs[PKR_LAYERS][RGB_MATRIX_LED_COUNT][2];
uint8_t        pkr_v[PKR_LAYERS][RGB_MATRIX_LED_COUNT];
static uint8_t pkr_transparent[PKR_LAYERS][PKR_MASK_BYTES]; // bit set = show the layer below

_Static_assert(RGB_MATRIX_LED_COUNT <= 255, "VIA per-key protocol uses an 8-bit LED index");
_Static_assert(PKR_LAYERS >= 1 && PKR_LAYERS <= MAX_LAYER, "VIA_PER_KEY_LAYERS out of range");
_Static_assert(EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_SIZE >= sizeof(pkr_hs) + sizeof(pkr_v) + sizeof(pkr_transparent), "EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_SIZE too small");
_Static_assert(RGB_MATRIX_COMMUNITY_MODULE_PER_KEY_VIA == VIA_PER_KEY_EFFECT_ID,
               "Direct effect ID differs from VIA_PER_KEY_EFFECT_ID; set it in the keymap's config.h and use the same --direct-id for the VIA definition");

static bool     pkr_save_pending = false;
static uint32_t pkr_save_timer   = 0;

static bool pkr_is_transparent(uint8_t layer, uint8_t led) {
    return layer > 0 && (pkr_transparent[layer][led / 8] & (1 << (led % 8)));
}

static void pkr_set_transparent(uint8_t layer, uint8_t led, bool transparent) {
    if (transparent && layer > 0) {
        pkr_transparent[layer][led / 8] |= 1 << (led % 8);
    } else {
        pkr_transparent[layer][led / 8] &= ~(1 << (led % 8));
    }
}

// Highest active layer where this LED isn't transparent
uint8_t pkr_layer_for(uint8_t led, layer_state_t state) {
    for (uint8_t layer = PKR_LAYERS - 1; layer > 0; layer--) {
        if (layer_state_cmp(state, layer) && !pkr_is_transparent(layer, led)) return layer;
    }
    return 0;
}

#ifdef SPLIT_KEYBOARD
// [layer, start, count, h0, s0, v0, t0, h1, ...] must fit in one RPC packet
#    define PKR_CHUNK_LEDS ((RPC_M2S_BUFFER_SIZE - 3) / 4)
#    define PKR_CHUNKS_PER_LAYER ((RGB_MATRIX_LED_COUNT + PKR_CHUNK_LEDS - 1) / PKR_CHUNK_LEDS)
#    define PKR_CHUNKS (PKR_LAYERS * PKR_CHUNKS_PER_LAYER)

static uint8_t  pkr_dirty[(PKR_CHUNKS + 7) / 8];
static uint16_t pkr_refresh_chunk = 0;
static uint8_t  pkr_idle_ticks    = 0;
static uint32_t pkr_sync_timer    = 0;

static void pkr_mark_dirty(uint8_t layer, uint8_t start, uint8_t count) {
    for (uint8_t i = start; i < start + count; i++) {
        uint16_t chunk = layer * PKR_CHUNKS_PER_LAYER + i / PKR_CHUNK_LEDS;
        pkr_dirty[chunk / 8] |= 1 << (chunk % 8);
    }
}

static bool pkr_send_chunk(uint16_t chunk) {
    uint8_t buf[RPC_M2S_BUFFER_SIZE];
    uint8_t layer = chunk / PKR_CHUNKS_PER_LAYER;
    uint8_t start = (chunk % PKR_CHUNKS_PER_LAYER) * PKR_CHUNK_LEDS;
    uint8_t count = MIN(PKR_CHUNK_LEDS, RGB_MATRIX_LED_COUNT - start);
    buf[0]        = layer;
    buf[1]        = start;
    buf[2]        = count;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t *p = &buf[3 + i * 4];
        p[0]       = pkr_hs[layer][start + i][0];
        p[1]       = pkr_hs[layer][start + i][1];
        p[2]       = pkr_v[layer][start + i];
        p[3]       = pkr_is_transparent(layer, start + i);
    }
    return transaction_rpc_send(PUT_VIA_PER_KEY_RGB, 3 + count * 4, buf);
}

static void pkr_slave_handler(uint8_t in_len, const void *in_data, uint8_t out_len, void *out_data) {
    const uint8_t *d = (const uint8_t *)in_data;
    if (in_len < 3) return;
    uint8_t layer = d[0], start = d[1], count = d[2];
    if (layer >= PKR_LAYERS || start + count > RGB_MATRIX_LED_COUNT || in_len < 3 + count * 4) return;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p            = &d[3 + i * 4];
        pkr_hs[layer][start + i][0] = p[0];
        pkr_hs[layer][start + i][1] = p[1];
        pkr_v[layer][start + i]     = p[2];
        pkr_set_transparent(layer, start + i, p[3]);
    }
}
#else
static inline void pkr_mark_dirty(uint8_t layer, uint8_t start, uint8_t count) {}
#endif

static void pkr_read(void) {
    eeconfig_read_via_per_key_rgb_datablock(pkr_hs, 0, sizeof(pkr_hs));
    eeconfig_read_via_per_key_rgb_datablock(pkr_v, sizeof(pkr_hs), sizeof(pkr_v));
    eeconfig_read_via_per_key_rgb_datablock(pkr_transparent, sizeof(pkr_hs) + sizeof(pkr_v), sizeof(pkr_transparent));
}

static void pkr_write(void) {
    eeconfig_update_via_per_key_rgb_datablock(pkr_hs, 0, sizeof(pkr_hs));
    eeconfig_update_via_per_key_rgb_datablock(pkr_v, sizeof(pkr_hs), sizeof(pkr_v));
    eeconfig_update_via_per_key_rgb_datablock(pkr_transparent, sizeof(pkr_hs) + sizeof(pkr_v), sizeof(pkr_transparent));
}

// Fresh/cleared EEPROM: base layer red at full brightness, higher layers transparent
void eeconfig_init_via_per_key_rgb_datablock(void) {
    for (uint8_t layer = 0; layer < PKR_LAYERS; layer++) {
        for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
            pkr_hs[layer][i][0] = 0;   // hue: red
            pkr_hs[layer][i][1] = 255; // sat
            pkr_v[layer][i]     = 255;
        }
        memset(pkr_transparent[layer], layer > 0 ? 0xFF : 0, PKR_MASK_BYTES);
        pkr_mark_dirty(layer, 0, RGB_MATRIX_LED_COUNT);
    }
    pkr_write();
}

void keyboard_post_init_via_per_key_rgb(void) {
    pkr_read();
#ifdef SPLIT_KEYBOARD
    memset(pkr_dirty, 0xFF, sizeof(pkr_dirty)); // push everything once at boot
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

    for (uint16_t chunk = 0; chunk < PKR_CHUNKS; chunk++) {
        if (pkr_dirty[chunk / 8] & (1 << (chunk % 8))) {
            if (pkr_send_chunk(chunk)) pkr_dirty[chunk / 8] &= ~(1 << (chunk % 8));
            return;
        }
    }
    if (++pkr_idle_ticks >= 25) {
        // Slow round-robin refresh (~1 chunk / 250 ms) covers a slave half that
        // was plugged in or reset after boot.
        pkr_idle_ticks = 0;
        pkr_send_chunk(pkr_refresh_chunk);
        pkr_refresh_chunk = (pkr_refresh_chunk + 1) % PKR_CHUNKS;
    }
#endif
}

static void pkr_underglow_command(uint8_t *data, uint8_t length) {
    uint8_t *command_id = &data[0];
    if (length < 5 || (*command_id != id_custom_set_value && *command_id != id_custom_get_value)) {
        *command_id = id_unhandled;
        return;
    }
    bool first = true;
    for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
        if (!HAS_FLAGS(g_led_config.flags[i], LED_FLAG_UNDERGLOW)) continue;
        if (*command_id == id_custom_set_value) {
            pkr_hs[0][i][0] = data[3];
            pkr_hs[0][i][1] = data[4];
            pkr_v[0][i]     = 255;
            pkr_mark_dirty(0, i, 1);
        } else if (first) {
            data[3] = pkr_hs[0][i][0];
            data[4] = pkr_hs[0][i][1];
        }
        first = false;
    }
}

static void pkr_layered_command(uint8_t *data, uint8_t length) {
    uint8_t *command_id = &data[0];
    uint8_t  layer      = data[3];
    uint8_t  start      = data[4];
    uint8_t  count      = data[5];
    if (layer >= PKR_LAYERS || start + count > RGB_MATRIX_LED_COUNT || 6 + count * 4 > length) {
        *command_id = id_unhandled;
        return;
    }
    for (uint8_t i = 0; i < count; i++) {
        uint8_t *p   = &data[6 + i * 4];
        uint8_t  led = start + i;
        switch (*command_id) {
            case id_custom_set_value:
                pkr_hs[layer][led][0] = p[0];
                pkr_hs[layer][led][1] = p[1];
                pkr_v[layer][led]     = p[2];
                pkr_set_transparent(layer, led, p[3]);
                break;
            case id_custom_get_value:
                p[0] = pkr_hs[layer][led][0];
                p[1] = pkr_hs[layer][led][1];
                p[2] = pkr_v[layer][led];
                p[3] = pkr_is_transparent(layer, led);
                break;
            default:
                *command_id = id_unhandled;
                return;
        }
    }
    if (*command_id == id_custom_set_value) pkr_mark_dirty(layer, start, count);
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

    switch (value_id) {
        case PKR_UNDERGLOW_VALUE_ID:
            pkr_underglow_command(data, length);
            return;
        case PKR_LAYERED_VALUE_ID:
            pkr_layered_command(data, length);
            return;
        case PKR_LAYER_COUNT_VALUE_ID:
            if (*command_id == id_custom_get_value) {
                data[3] = PKR_LAYERS;
            } else {
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
                memcpy(&pkr_v[0][start], &data[5], count);
                pkr_mark_dirty(0, start, count);
                break;
            case id_custom_get_value:
                memcpy(&data[5], &pkr_v[0][start], count);
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
            memcpy(&pkr_hs[0][start], &data[5], count * 2);
            memset(&pkr_v[0][start], 255, count);
            pkr_mark_dirty(0, start, count);
            break;
        case id_custom_get_value:
            memcpy(&data[5], &pkr_hs[0][start], count * 2);
            break;
        default:
            *command_id = id_unhandled;
            break;
    }
}
