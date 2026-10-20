// SPDX-License-Identifier: GPL-2.0-or-later
// Becomes RGB_MATRIX_COMMUNITY_MODULE_PER_KEY_VIA in enum rgb_matrix_effects
RGB_MATRIX_EFFECT(PER_KEY_VIA)

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS
extern uint8_t pkr_hs[RGB_MATRIX_LED_COUNT][2];
extern uint8_t pkr_v[RGB_MATRIX_LED_COUNT];

static bool PER_KEY_VIA(effect_params_t *params) {
    RGB_MATRIX_USE_LIMITS(led_min, led_max);
    uint8_t val = rgb_matrix_get_val();
    for (uint8_t i = led_min; i < led_max; i++) {
        RGB_MATRIX_TEST_LED_FLAGS();
        hsv_t hsv = {pkr_hs[i][0], pkr_hs[i][1], scale8(val, pkr_v[i])};
        rgb_t rgb = rgb_matrix_hsv_to_rgb(hsv);
        rgb_matrix_set_color(i, rgb.r, rgb.g, rgb.b);
    }
    return rgb_matrix_check_finished_leds(led_max);
}
#endif
