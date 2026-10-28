/* Copyright 2021 Danny Nguyen <danny@keeb.io>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#if defined(KEYBOARD_keebio_sinc_rev1) || defined(KEYBOARD_keebio_sinc_rev2)

    #define VIA_EEPROM_LAYOUT_OPTIONS_SIZE 2

#endif

#if defined(KEYBOARD_keebio_sinc_rev3)

    // Rev. 3's effect list puts Direct outside VIA's stock effect menu
    #define VIA_PER_KEY_RGB_DISABLE

#endif

#if defined(KEYBOARD_keebio_sinc_rev4)

    // Per-key lighting layers and indicators for 117 LEDs take ~2.9 KB, leaving
    // little of the default 4 KB of emulated EEPROM for VIA macros. 16 KB of flash
    // backing gives 8 KB of EEPROM.
    #define WEAR_LEVELING_BACKING_SIZE 16384

#endif
