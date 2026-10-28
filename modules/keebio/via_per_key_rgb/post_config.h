// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Included after the keyboard and keymap config.h, so RGB_MATRIX_TIMEOUT is final here
#if defined(RGB_MATRIX_ENABLE) && !defined(VIA_PER_KEY_RGB_DISABLE) && !defined(__ASSEMBLER__)
#    include <stdint.h>

// RGB_MATRIX_TIMEOUT becomes the default for the RGB sleep timer VIA can change. An enum
// captures its value now; a macro would only expand after the #undef below.
enum {
    VIA_PER_KEY_TIMEOUT_DEFAULT_S =
#    ifdef RGB_MATRIX_TIMEOUT
        (RGB_MATRIX_TIMEOUT) / 1000
#    else
        0
#    endif
};

// QMK's timeout check then reads the runtime value (0 = no timeout). In QMK's
// "#if RGB_MATRIX_TIMEOUT > 0" the variable reads as 0 and the fallback keeps the
// check compiled in; at runtime the fallback never times out.
#    undef RGB_MATRIX_TIMEOUT
extern uint32_t pkr_timeout_ms;
#    define RGB_MATRIX_TIMEOUT (pkr_timeout_ms ? pkr_timeout_ms : 0xFFFFFFFF)

#endif
