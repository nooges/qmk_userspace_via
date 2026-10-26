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
//   id_custom_get_value  [0x08, 0, 5]  -> lighting layers at data[3], indicators at data[4],
//                                          LED count at data[5]
//   id_custom_get_value  [0x08, 0, 6, led_index, count]  -> LED flags (LED_FLAG_*) at data[5..],
//                                          so VIA can find underglow LEDs to paint per layer
// Layer 0 ignores the transparent byte.
//
// RGB sleep timer: lighting turns off after this many minutes without input
// (0 = never). Stored in ms; RGB_MATRIX_TIMEOUT is the default, and post_config.h
// points QMK's timeout check at the stored value. VIA shows a preset dropdown plus
// a slider for other values:
//   id_custom_set_value  [0x07, 0, 7, minutes]  -> preset; PKR_SLEEP_CUSTOM leaves it as is
//   id_custom_get_value  [0x08, 0, 7]  -> minutes at data[3] if a preset, else PKR_SLEEP_CUSTOM
//   id_custom_set_value  [0x07, 0, 8, minutes_hi, minutes_lo]  -> any value up to PKR_SLEEP_MAX
//   id_custom_get_value  [0x08, 0, 8]  -> minutes at data[3..4] (big-endian), rounded up
//
// Indicator overlays follow the lighting layers (value ID 4 layer index
// VIA_PER_KEY_LAYERS + n) for Caps Lock, Num Lock, Scroll Lock and Caps Word. While a
// state is on, its non-transparent LEDs are drawn over whatever effect is running;
// later indicators win where they overlap.
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
#include <lib/lib8tion/lib8tion.h>

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 1, 3);

// Does nothing on boards without RGB matrix, so a VIA keymap shared with an RGB
// light revision still builds
#ifdef RGB_MATRIX_ENABLE

// Defined (weak) in rgb_matrix.c but not declared in its header
rgb_t rgb_matrix_hsv_to_rgb(hsv_t hsv);

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
#define PKR_LED_FLAGS_VALUE_ID 6
#define PKR_SLEEP_VALUE_ID 7
#define PKR_SLEEP_CUSTOM_VALUE_ID 8

// Must match the dropdown options in add_per_key_rgb.py
static const uint8_t pkr_sleep_presets[] = {0, 5, 10, 15, 30, 60, 120};
#define PKR_SLEEP_CUSTOM 255
// Custom slider max; VIA sends ranges above 255 as 16-bit big-endian
#define PKR_SLEEP_MAX 1440

#define PKR_LAYERS VIA_PER_KEY_LAYERS
#define PKR_MASK_BYTES ((RGB_MATRIX_LED_COUNT + 7) / 8)

enum { PKR_IND_CAPS_LOCK, PKR_IND_NUM_LOCK, PKR_IND_SCROLL_LOCK, PKR_IND_CAPS_WORD, PKR_INDICATORS };
#define PKR_OVERLAYS (PKR_LAYERS + PKR_INDICATORS)

// Read by the effect in rgb_matrix_module.inc
uint8_t        pkr_hs[PKR_OVERLAYS][RGB_MATRIX_LED_COUNT][2];
uint8_t        pkr_v[PKR_OVERLAYS][RGB_MATRIX_LED_COUNT];
static uint8_t pkr_transparent[PKR_OVERLAYS][PKR_MASK_BYTES]; // bit set = show the layer below

// Read by QMK's timeout check through RGB_MATRIX_TIMEOUT (see post_config.h)
uint32_t pkr_timeout_ms = 0;

// Bit per indicator that's on. The master reads it locally and sends it to the slave.
static uint8_t pkr_indicator_state = 0;

_Static_assert(RGB_MATRIX_LED_COUNT <= 255, "VIA per-key protocol uses an 8-bit LED index");
_Static_assert(PKR_LAYERS >= 1 && PKR_LAYERS <= MAX_LAYER, "VIA_PER_KEY_LAYERS out of range");
_Static_assert(EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_SIZE >= sizeof(pkr_hs) + sizeof(pkr_v) + sizeof(pkr_transparent) + sizeof(pkr_timeout_ms), "EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_SIZE too small");
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

static uint8_t pkr_local_indicator_state(void) {
    led_t   leds  = host_keyboard_led_state();
    uint8_t state = (leds.caps_lock << PKR_IND_CAPS_LOCK) | (leds.num_lock << PKR_IND_NUM_LOCK) | (leds.scroll_lock << PKR_IND_SCROLL_LOCK);
#ifdef CAPS_WORD_ENABLE
    state |= is_caps_word_on() << PKR_IND_CAPS_WORD;
#endif
    return state;
}

// Highest active layer where this LED isn't transparent
uint8_t pkr_layer_for(uint8_t led, layer_state_t state) {
    for (uint8_t layer = PKR_LAYERS - 1; layer > 0; layer--) {
        if (layer_state_cmp(state, layer) && !pkr_is_transparent(layer, led)) return layer;
    }
    return 0;
}

#ifdef SPLIT_KEYBOARD
// [layer, start, count, h0, s0, v0, t0, h1, ...] must fit in one RPC packet.
// Indicator state and timeout go as [PKR_STATE_PACKET, state, timeout_ms (4 bytes)].
#    define PKR_STATE_PACKET 0xFF
#    define PKR_CHUNK_LEDS ((RPC_M2S_BUFFER_SIZE - 3) / 4)
#    define PKR_CHUNKS_PER_LAYER ((RGB_MATRIX_LED_COUNT + PKR_CHUNK_LEDS - 1) / PKR_CHUNK_LEDS)
#    define PKR_CHUNKS (PKR_OVERLAYS * PKR_CHUNKS_PER_LAYER)
_Static_assert(PKR_OVERLAYS < PKR_STATE_PACKET, "Too many overlays for the sync packet");

static uint8_t  pkr_dirty[(PKR_CHUNKS + 7) / 8];
static uint16_t pkr_refresh_chunk = 0;
static uint8_t  pkr_idle_ticks    = 0;
static uint32_t pkr_sync_timer    = 0;
static uint16_t pkr_sent_state    = 0xFFFF; // never sent
static uint32_t pkr_sent_timeout  = 0;

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
    if (in_len >= 6 && d[0] == PKR_STATE_PACKET) {
        pkr_indicator_state = d[1];
        memcpy(&pkr_timeout_ms, &d[2], sizeof(pkr_timeout_ms));
        return;
    }
    if (in_len < 3) return;
    uint8_t layer = d[0], start = d[1], count = d[2];
    if (layer >= PKR_OVERLAYS || start + count > RGB_MATRIX_LED_COUNT || in_len < 3 + count * 4) return;
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
    eeconfig_read_via_per_key_rgb_datablock(&pkr_timeout_ms, sizeof(pkr_hs) + sizeof(pkr_v) + sizeof(pkr_transparent), sizeof(pkr_timeout_ms));
}

static void pkr_write(void) {
    eeconfig_update_via_per_key_rgb_datablock(pkr_hs, 0, sizeof(pkr_hs));
    eeconfig_update_via_per_key_rgb_datablock(pkr_v, sizeof(pkr_hs), sizeof(pkr_v));
    eeconfig_update_via_per_key_rgb_datablock(pkr_transparent, sizeof(pkr_hs) + sizeof(pkr_v), sizeof(pkr_transparent));
    eeconfig_update_via_per_key_rgb_datablock(&pkr_timeout_ms, sizeof(pkr_hs) + sizeof(pkr_v) + sizeof(pkr_transparent), sizeof(pkr_timeout_ms));
}

// Fresh/cleared EEPROM: base layer red at full brightness, every overlay transparent,
// timeout from RGB_MATRIX_TIMEOUT
void eeconfig_init_via_per_key_rgb_datablock(void) {
    for (uint8_t layer = 0; layer < PKR_OVERLAYS; layer++) {
        for (uint8_t i = 0; i < RGB_MATRIX_LED_COUNT; i++) {
            pkr_hs[layer][i][0] = 0;   // hue: red
            pkr_hs[layer][i][1] = 255; // sat
            pkr_v[layer][i]     = 255;
        }
        memset(pkr_transparent[layer], layer > 0 ? 0xFF : 0, PKR_MASK_BYTES);
        pkr_mark_dirty(layer, 0, RGB_MATRIX_LED_COUNT);
    }
    pkr_timeout_ms = (uint32_t)VIA_PER_KEY_TIMEOUT_DEFAULT_S * 1000;
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

    if (is_keyboard_master()) {
        pkr_indicator_state = pkr_local_indicator_state();
    }

#ifdef SPLIT_KEYBOARD
    if (!is_keyboard_master() || !is_transport_connected()) return;
    if (timer_elapsed32(pkr_sync_timer) < 10) return;
    pkr_sync_timer = timer_read32();

    if (pkr_sent_state != pkr_indicator_state || pkr_sent_timeout != pkr_timeout_ms) {
        uint8_t buf[6] = {PKR_STATE_PACKET, pkr_indicator_state};
        memcpy(&buf[2], &pkr_timeout_ms, sizeof(pkr_timeout_ms));
        if (transaction_rpc_send(PUT_VIA_PER_KEY_RGB, sizeof(buf), buf)) {
            pkr_sent_state   = pkr_indicator_state;
            pkr_sent_timeout = pkr_timeout_ms;
        }
        return;
    }

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
        if (pkr_refresh_chunk == 0) pkr_sent_state = 0xFFFF; // resend state each pass too
    }
#endif
}

bool rgb_matrix_indicators_advanced_via_per_key_rgb(uint8_t led_min, uint8_t led_max) {
    if (!pkr_indicator_state) return true;
    uint8_t val = rgb_matrix_get_val();
    for (uint8_t ind = 0; ind < PKR_INDICATORS; ind++) {
        if (!(pkr_indicator_state & (1 << ind))) continue;
        uint8_t overlay = PKR_LAYERS + ind;
        for (uint8_t i = led_min; i < led_max; i++) {
            if (pkr_is_transparent(overlay, i)) continue;
            hsv_t hsv = {pkr_hs[overlay][i][0], pkr_hs[overlay][i][1], scale8(val, pkr_v[overlay][i])};
            rgb_t rgb = rgb_matrix_hsv_to_rgb(hsv);
            rgb_matrix_set_color(i, rgb.r, rgb.g, rgb.b);
        }
    }
    return true;
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
    if (layer >= PKR_OVERLAYS || start + count > RGB_MATRIX_LED_COUNT || 6 + count * 4 > length) {
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

static void pkr_sleep_command(uint8_t *data) {
    uint8_t *command_id = &data[0];
    bool     dropdown   = data[2] == PKR_SLEEP_VALUE_ID;
    if (*command_id == id_custom_set_value) {
        if (dropdown && data[3] == PKR_SLEEP_CUSTOM) return; // keep the value; VIA shows the slider
        uint16_t minutes = dropdown ? data[3] : MIN((data[3] << 8) | data[4], PKR_SLEEP_MAX);
        pkr_timeout_ms   = (uint32_t)minutes * 60000;
        // Restart the countdown (and wake the lighting) as if a key was pressed;
        // VIA traffic doesn't count as input activity
        set_activity_timestamps(sync_timer_read32(), last_encoder_activity_time(), last_pointing_device_activity_time());
        // Don't rely on VIA sending id_custom_save after these controls
        pkr_save_pending = true;
        pkr_save_timer   = timer_read32();
    } else if (*command_id == id_custom_get_value) {
        uint16_t minutes = MIN((pkr_timeout_ms + 59999) / 60000, PKR_SLEEP_MAX);
        if (!dropdown) {
            data[3] = minutes >> 8;
            data[4] = minutes & 0xFF;
            return;
        }
        data[3] = PKR_SLEEP_CUSTOM;
        for (uint8_t i = 0; i < ARRAY_SIZE(pkr_sleep_presets); i++) {
            if (pkr_sleep_presets[i] == minutes) data[3] = minutes;
        }
    } else {
        *command_id = id_unhandled;
    }
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
                data[4] = PKR_INDICATORS;
                data[5] = RGB_MATRIX_LED_COUNT;
            } else {
                *command_id = id_unhandled;
            }
            return;
        case PKR_SLEEP_VALUE_ID:
        case PKR_SLEEP_CUSTOM_VALUE_ID:
            pkr_sleep_command(data);
            return;
        case PKR_LED_FLAGS_VALUE_ID: {
            uint8_t start = data[3], count = data[4];
            if (*command_id != id_custom_get_value || start + count > RGB_MATRIX_LED_COUNT || 5 + count > length) {
                *command_id = id_unhandled;
                return;
            }
            for (uint8_t i = 0; i < count; i++) {
                data[5 + i] = g_led_config.flags[start + i];
            }
            return;
        }
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

#endif // RGB_MATRIX_ENABLE
