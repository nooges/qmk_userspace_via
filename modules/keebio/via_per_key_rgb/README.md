# VIA Per-Key RGB

QMK community module that adds VIA's key painter (per-key hue/sat) to RGB matrix boards, plus a single "Underglow Color" control for LEDs that have no key.

It adds a "Direct" RGB matrix effect, answers VIA's per-key commands on custom channel 0, saves colors to its own EEPROM datablock (debounced), and on split boards mirrors the colors to the slave half.

## Adding it to a board

1. Add `keymap.json` next to the board's VIA `keymap.c`:

   ```json
   {
       "modules": ["keebio/via_per_key_rgb"]
   }
   ```

2. Build. The build fails if the Direct effect ID isn't 45 (boards that enable exactly the effects in VIA's built-in `qmk_rgb_matrix` menu). If it fails, add `#define VIA_PER_KEY_EFFECT_ID <N>` to the keymap's `config.h`, where N is 1 + the number of enabled core effects.

3. Patch the VIA definition (in a `the-via/keyboards` checkout):

   ```sh
   qmk info -kb keebio/<board>/<rev> -f json > /tmp/info.json
   python3 <path to this folder>/add_per_key_rgb.py v3/keebio/<family>/<board>.json /tmp/info.json \
       --direct-id <N> -o v3/keebio/<family>/<board>.json
   npx prettier --write v3/keebio/<family>/<board>.json
   ```

   The script replaces the `qmk_rgb_matrix` menu with one that adds "Direct (Per-Key)", the key painter and (if the board has underglow LEDs) "Underglow Color"; switches `qmk_lighting` keycodes to `qmk_rgb_matrix_keycodes`; and tags every key with its LED index.

## Notes

- The keyboard must not define `via_custom_value_command_kb`; this module does.
- The module reports `VIA_FIRMWARE_VERSION 1`. The generated menu shows the stock lighting controls below that and the per-key controls at 1 or later, so one definition works with old and new firmware. Boards that set their own `VIA_FIRMWARE_VERSION` must keep it at 1 or higher.
- VIA reads every LED's color on connect when keys carry LED tags. The app must skip that read when the color-palette control is firmware-gated away (keebio/via-app `menusSlice.ts`); without that, old firmware still fails to load the Lighting menu.
- Adding the module moves the dynamic keymap in EEPROM, so VIA settings reset on update.
- Colors default to red on first boot and after `EE_CLR`.
- Each LED also stores a brightness (0 = off, custom value ID 3), scaled by the global brightness. Painting a color resets it to full, so stock VIA can still turn an LED back on. Turning LEDs off from the painter needs keebio/via-app (the "Off" swatch).
- Lighting layers (`VIA_PER_KEY_LAYERS`, default 4): layer 0 is the base paint, and each higher layer overlays it while that keymap layer is active. Keys painted transparent on a layer show the next active layer below, like `KC_TRNS`. Stock VIA only edits layer 0; keebio/via-app has layer tabs and a "Transparent" brush. Split boards get `SPLIT_LAYER_STATE_ENABLE` so the slave half follows the layer.
- Indicators: Caps Lock, Num Lock, Scroll Lock and Caps Word each have a paintable overlay (all transparent by default). While a state is on, its painted keys are drawn over any effect, not just Direct. Painting them needs keebio/via-app (indicator buttons in the painter). On split boards the master sends indicator state to the slave itself, since QMK doesn't sync Caps Word.
- Storage is `(VIA_PER_KEY_LAYERS + 4) × (3 bytes per LED + 1 bit per LED)`: about 1.7 KB for 4 layers on a 68-LED board. RP2040 boards have 4 KB of emulated EEPROM by default. STM32 boards default to 1 KB and need more, e.g. `#define WEAR_LEVELING_BACKING_SIZE 6144` in the keymap's `config.h` as Iris LM does. Wear leveling keeps a RAM copy of the whole EEPROM, so check RAM still fits (the G431 tops out around 3 KB), or use fewer layers.
- Bump `EECONFIG_MODULE_VIA_PER_KEY_RGB_DATA_VERSION` in `config.h` if the stored layout changes.
