# VIA Per-Key RGB

QMK community module that lets VIA paint per-key lighting on RGB matrix boards: per-key color and brightness, lighting that changes with the active layer, underglow, and status indicators. It works with stock VIA for the basics; the full painter needs [keebio/via-app](https://github.com/keebio/via-app).

## Features

| Feature | Stock VIA | keebio/via-app |
| --- | --- | --- |
| **Direct (Per-Key) effect**: renders the painted colors, scaled by the global brightness | ✓ | ✓ |
| **Per-key color**: paint keys in the key painter | ✓ (base layer) | ✓ |
| **Per-key brightness and Off**: brightness is the vertical axis of the color picker; the Off swatch turns keys off | | ✓ |
| **Lighting layers**: each keymap layer can overlay the base paint while it's active; unpainted keys are transparent and show the layer below, like `KC_TRNS` | | ✓ (painter follows VIA's layer selector; Transparent brush) |
| **Underglow**: LEDs without a key | base-layer color only ("Underglow Color" control) | color, brightness, Off and Transparent per layer and indicator ("Underglow" row in the painter) |
| **Status indicators**: Caps Lock, Num Lock, Scroll Lock and Caps Word each have a paintable overlay, drawn over **any** effect while the state is on | | ✓ (indicator buttons in the Direct painter; "Status Indicators" painter for other effects) |
| **Lighting export/import**: lighting settings and every painted layer and indicator, as a `.lighting.json` file separate from the keymap | | ✓ (Save + Load pane) |

Also:

- **Persistence**: everything is saved to the module's own EEPROM datablock. Saves are debounced (1.5 s after the last change) so painting doesn't wear out emulated EEPROM.
- **Split boards**: the master mirrors all lighting data and indicator state to the slave half, since VIA only talks to the master and QMK doesn't sync Caps Word. The module turns on `SPLIT_LAYER_STATE_ENABLE` so the slave follows the active layer.
- **Defaults**: the base layer starts red at full brightness; every other layer and indicator starts transparent. These defaults are restored after `EE_CLR` or a storage version change.
- **Caps Word**: the module enables `CAPS_WORD_ENABLE` so the Caps Word indicator works. Assign `CW_TOGG` in VIA.
- **Boards without RGB matrix**: the module builds to nothing (apart from Caps Word), so a VIA keymap shared with an RGB light revision still builds.

## Adding it to a board

1. Add `keymap.json` next to the board's VIA `keymap.c`:

   ```json
   {
       "modules": ["keebio/via_per_key_rgb"]
   }
   ```

2. Build. The build fails if the Direct effect ID isn't 45, which is what boards get when they enable exactly the effects in VIA's built-in `qmk_rgb_matrix` menu. If it fails, add `#define VIA_PER_KEY_EFFECT_ID <N>` to the keymap's `config.h`, where N is 2 + the number of animations enabled in `keyboard.json` (All Off and Solid Color come first).

3. Check EEPROM space (see [Storage](#storage)). STM32 boards usually need a bigger emulated EEPROM.

4. Patch the VIA definition in a `the-via/keyboards` checkout:

   ```sh
   qmk info -kb keebio/<board>/<rev> -f json > /tmp/info.json
   python3 <path to this folder>/add_per_key_rgb.py v3/keebio/<family>/<board>.json /tmp/info.json \
       --direct-id <N> -o v3/keebio/<family>/<board>.json
   npx prettier --write v3/keebio/<family>/<board>.json
   ```

   The script:

   - replaces the `qmk_rgb_matrix` menu with one that keeps the stock controls for old firmware and, on firmware with this module, adds "Direct (Per-Key)", the key painter, "Underglow Color" (if the board has underglow LEDs) and "Status Indicators" for other effects
   - switches `qmk_lighting` keycodes to `qmk_rgb_matrix_keycodes`
   - tags every key with its LED index from the board's `rgb_matrix.layout`

   It only handles boards whose enabled effects match VIA's built-in list (Direct ID 45 or higher); other boards need the effect dropdown written by hand.

## Compatibility

The module reports `VIA_FIRMWARE_VERSION 2`, and the generated definition only shows (and reads) the controls each firmware supports:

| Firmware version | Gets |
| --- | --- |
| 0 (without the module) | stock lighting controls |
| 1 | per-key colors and underglow color |
| 2 | lighting layers, brightness, indicators and Status Indicators |

So the updated definition can ship before customers update their firmware. Boards that set their own `VIA_FIRMWARE_VERSION` must keep it at 2 or higher.

VIA reads every LED's color on connect when keys carry LED tags. keebio/via-app skips that read when the painter is firmware-gated away; stock VIA doesn't, so on stock VIA old firmware can still fail to load the Lighting menu with the new definition.

## Storage

`(VIA_PER_KEY_LAYERS + 4) × (3 bytes per LED + 1 bit per LED)`: about 1.7 KB for 4 layers on a 68-LED board, 280 bytes on BDN9.

- **RP2040**: 4 KB of emulated EEPROM by default; fits.
- **STM32**: 1 KB by default. Raise it in the keymap's `config.h`, e.g. `#define WEAR_LEVELING_BACKING_SIZE 6144` (3 KB of EEPROM) as Iris LM does. Wear leveling keeps a RAM copy of the whole EEPROM, so check the build still links; the G431 tops out around 3 KB. Otherwise use fewer layers (`VIA_PER_KEY_LAYERS`).
- **AVR**: 1 KB total; likely too small for lighting layers.

Adding the module, or changing the storage layout, moves VIA's dynamic keymap in EEPROM, so VIA settings reset on that update. Bump `EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_VERSION` in `config.h` whenever the stored layout changes.

## Configuration

| Define | Default | |
| --- | --- | --- |
| `VIA_PER_KEY_EFFECT_ID` | 45 | Direct's effect ID; must match the definition's `--direct-id` |
| `VIA_PER_KEY_LAYERS` | 4 | Lighting layers (keymap layers 0 to N-1) |
| `VIA_PER_KEY_SAVE_DELAY_MS` | 1500 | Delay before saving after the last change |

## Protocol

All on VIA custom channel 0 (`id_custom_set_value` 0x07 / `id_custom_get_value` 0x08 / `id_custom_save` 0x09):

| Value ID | Data | |
| --- | --- | --- |
| 1 | `led, count, (hue, sat)…` | Base-layer color; stock VIA's key painter. Setting it resets brightness to full. |
| 2 | `hue, sat` | Base-layer underglow color, all underglow LEDs at once |
| 3 | `led, count, val…` | Base-layer brightness (0 = off) |
| 4 | `overlay, led, count, (hue, sat, val, transparent)…` | Any lighting layer or indicator overlay (overlay = layer, or `VIA_PER_KEY_LAYERS` + indicator) |
| 5 | get: `layers, indicators, led count` | |
| 6 | get `led, count` → LED flags | Lets the app find underglow LEDs |

`id_custom_save` schedules a debounced save. Packet layouts are documented at the top of `via_per_key_rgb.c`.

## Notes

- The keyboard must not define `via_custom_value_command_kb`; this module does.
