// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef VIA_PER_KEY_RGB_DISABLE
// Becomes RGB_MATRIX_COMMUNITY_MODULE_PER_KEY_VIA in enum rgb_matrix_effects
RGB_MATRIX_EFFECT(PER_KEY_VIA)

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS
// Lighting layers, then indicator overlays; the effect only reads the layers
extern uint8_t pkr_hs[][RGB_MATRIX_LED_COUNT][2];
extern uint8_t pkr_v[][RGB_MATRIX_LED_COUNT];
uint8_t        pkr_layer_for(uint8_t led, layer_state_t state);

static bool PER_KEY_VIA(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);
    uint8_t       val   = rgb_matrix_get_val();
    layer_state_t state = layer_state | default_layer_state;
    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();
        uint8_t layer = pkr_layer_for(i, state);
        hsv_t   hsv   = {pkr_hs[layer][i][0], pkr_hs[layer][i][1], scale8(val, pkr_v[layer][i])};
        rgb_t   rgb   = rgb_matrix_hsv_to_rgb(hsv);
        rgb_matrix_set_color(i, rgb.r, rgb.g, rgb.b);
    }
    return rgb_matrix_check_finished_leds(led_max);
}
#endif
#endif // VIA_PER_KEY_RGB_DISABLE
