#!/usr/bin/env python3
"""
Add VIA per-key RGB painting to a VIA v3 definition.

  1. Replaces the built-in "qmk_rgb_matrix" menu with an explicit copy that has a
     "Direct" effect and a color-palette control (VIA only shows the key painter
     when that control is visible).
  2. Tags every key with its LED index ("r,c\\nl<n>"), taken from the resolved
     rgb_matrix.layout of the QMK keyboard, so LED order never has to match matrix order.

Usage:
  qmk info -kb keebio/iris_ce/rev1 -f json > iris_ce_info.json
  python3 add_per_key_rgb.py iris-ce-rev1.json iris_ce_info.json --direct-id 45 -o out.json

--direct-id must equal VIA_PER_KEY_EFFECT_ID in the firmware (the firmware build
fails if that value is wrong, so build first and copy the number over).
"""
import argparse
import copy
import json
import re
import sys

ROWCOL = re.compile(r"^\s*(\d+)\s*,\s*(\d+)\s*$")
LED = re.compile(r"^[lL]\d+\s*$")

# VIA's built-in qmk_rgb_matrix menu (@the-via/reader common-menus/qmk_rgb_matrix.js)
VIA_RGB_MATRIX_EFFECTS = [
    "All Off", "Solid Color", "Alphas Mods", "Gradient Up/Down", "Gradient Left/Right",
    "Breathing", "Band Sat.", "Band Val.", "Pinwheel Sat.", "Pinwheel Val.", "Spiral Sat.",
    "Spiral Val.", "Cycle All", "Cycle Left/Right", "Cycle Up/Down", "Rainbow Moving Chevron",
    "Cycle Out/In", "Cycle Out/In Dual", "Cycle Pinwheel", "Cycle Spiral", "Dual Beacon",
    "Rainbow Beacon", "Rainbow Pinwheels", "Raindrops", "Jellybean Raindrops", "Hue Breathing",
    "Hue Pendulum", "Hue Wave", "Pixel Rain", "Pixel Flow", "Pixel Fractal", "Typing Heatmap",
    "Digital Rain", "Solid Reactive Simple", "Solid Reactive", "Solid Reactive Wide",
    "Solid Reactive Multi Wide", "Solid Reactive Cross", "Solid Reactive Multi Cross",
    "Solid Reactive Nexus", "Solid Reactive Multi Nexus", "Splash", "Multi Splash",
    "Solid Splash", "Solid Multi Splash",
]
NO_COLOR_EFFECTS = [24, 28, 29, 32]  # same exclusions VIA's built-in menu uses

# VIA_FIRMWARE_VERSION set by the module's config.h; older firmware gets the stock controls
PER_KEY_FIRMWARE_VERSION = 1
# Firmware with lighting layers and indicator overlays (value ID 5)
INDICATOR_FIRMWARE_VERSION = 2

# The painter in indicators-only mode, for effects other than Direct.
# keebio/via-app keys off this ID; value ID 5 (overlay counts) is a harmless read.
INDICATOR_CONTROL_ID = "id_via_per_key_indicators"


def effect_controls(options, no_color_ids, no_speed_ids):
    """Effect dropdown plus the Speed/Color controls, as in VIA's built-in menu."""
    e = "{id_qmk_rgb_matrix_effect}"
    no_speed = " && ".join(f"{e} != {n}" for n in [0] + no_speed_ids)
    no_color = " && ".join(f"{e} != {n}" for n in [0] + no_color_ids)
    return [
        {"label": "Effect", "type": "dropdown",
         "content": ["id_qmk_rgb_matrix_effect", 3, 2], "options": options},
        {"showIf": no_speed, "label": "Effect Speed", "type": "range", "options": [0, 255],
         "content": ["id_qmk_rgb_matrix_effect_speed", 3, 3]},
        {"showIf": no_color, "label": "Color", "type": "color",
         "content": ["id_qmk_rgb_matrix_color", 3, 4]},
    ]


def build_menu(direct_id, has_underglow):
    if direct_id < len(VIA_RGB_MATRIX_EFFECTS):
        sys.exit(f"--direct-id {direct_id} collides with built-in effect "
                 f"'{VIA_RGB_MATRIX_EFFECTS[direct_id]}'. This board's enabled effects "
                 "differ from VIA's built-in list; write the effect dropdown by hand.")
    stock = [[name, i] for i, name in enumerate(VIA_RGB_MATRIX_EFFECTS)]
    options = list(stock)
    for i in range(len(VIA_RGB_MATRIX_EFFECTS), direct_id):
        options.append([f"Effect {i}", i])  # placeholder for kb-level custom effects, if any
    options.append(["Direct (Per-Key)", direct_id])

    e = "{id_qmk_rgb_matrix_effect}"
    per_key = effect_controls(options, NO_COLOR_EFFECTS + [direct_id], [direct_id])
    per_key.append({"showIf": f"{e} == {direct_id}", "label": "Per-Key Colors",
                    "type": "color-palette", "content": ["id_qmk_rgb_matrix_color", 3, 4]})
    if has_underglow:
        # Firmware value ID 2 on custom channel 0 sets every underglow LED at once
        per_key.append({"showIf": f"{e} == {direct_id}", "label": "Underglow Color",
                        "type": "color", "content": ["id_via_per_key_underglow", 0, 2]})

    # Firmware-only showIf branches are pruned before VIA reads any values, so
    # firmware without the module never sees the channel 0 per-key requests.
    fw = "{id_firmware_version}"
    return {
        "label": "Lighting",
        "content": [{
            "label": "Backlight",
            "content": [
                {"label": "Brightness", "type": "range", "options": [0, 255],
                 "content": ["id_qmk_rgb_matrix_brightness", 3, 1]},
                {"showIf": f"{fw} < {PER_KEY_FIRMWARE_VERSION}",
                 "content": effect_controls(stock, NO_COLOR_EFFECTS, [])},
                {"showIf": f"{fw} >= {PER_KEY_FIRMWARE_VERSION}", "content": per_key},
                # Groups can't nest, so this sits beside the per-key group
                {"showIf": f"{fw} >= {INDICATOR_FIRMWARE_VERSION}", "content": [
                    {"showIf": f"{e} != {direct_id}", "label": "Status Indicators",
                     "type": "color-palette", "content": [INDICATOR_CONTROL_ID, 0, 5]},
                ]},
            ],
        }],
    }


def led_map_from_info(info):
    layout = info.get("rgb_matrix", {}).get("layout")
    if not layout:
        sys.exit("No rgb_matrix.layout in QMK info JSON (use `qmk info -kb <kb> -f json`).")
    m = {}
    for idx, led in enumerate(layout):
        if "matrix" in led:
            m.setdefault(tuple(led["matrix"]), idx)
    return m


def has_underglow_leds(info):
    return any(led.get("flags", 0) & 0x02 for led in info["rgb_matrix"]["layout"])  # LED_FLAG_UNDERGLOW


def tag_keys(keymap, led_map):
    a = 0  # VIA's KLE parser default; persists across rows
    tagged, missing, skipped = 0, [], []
    for row in keymap:
        for i, item in enumerate(row):
            if isinstance(item, dict):
                if "a" in item:
                    a = item["a"]
                continue
            parts = item.split("\n")
            mrc = ROWCOL.match(parts[0])
            if not mrc:
                continue  # decal / encoder-only label
            rc = (int(mrc.group(1)), int(mrc.group(2)))
            if a not in (0, 4):
                skipped.append(rc)  # legend slot 1 is not the LED slot for this alignment
                continue
            if rc not in led_map:
                missing.append(rc)
                continue
            while len(parts) < 2:
                parts.append("")
            if parts[1] and not LED.match(parts[1]):
                sys.exit(f"Key {rc} already uses legend slot 1 ({parts[1]!r}); resolve by hand.")
            parts[1] = f"l{led_map[rc]}"
            row[i] = "\n".join(parts)
            tagged += 1
    return tagged, missing, skipped


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("via_json")
    ap.add_argument("qmk_info_json")
    ap.add_argument("--direct-id", type=int, required=True)
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    via = json.load(open(args.via_json))
    info = json.load(open(args.qmk_info_json))

    menus = via.get("menus", [])
    if "qmk_rgb_matrix" not in menus:
        sys.exit("Definition does not use the built-in 'qmk_rgb_matrix' menu; add the "
                 "Direct option and color-palette control to its custom menu by hand.")
    via["menus"] = [build_menu(args.direct_id, has_underglow_leds(info)) if m == "qmk_rgb_matrix" else m for m in menus]
    # With the menu no longer a built-in string, VIA can't tell which lighting
    # keycodes (UG_* vs RM_*) apply, so name the RGB matrix module explicitly.
    via["keycodes"] = ["qmk_rgb_matrix_keycodes" if k == "qmk_lighting" else k
                       for k in via.get("keycodes", [])]

    keymap = copy.deepcopy(via["layouts"]["keymap"])
    tagged, missing, skipped = tag_keys(keymap, led_map_from_info(info))
    via["layouts"]["keymap"] = keymap

    with open(args.output, "w") as f:
        json.dump(via, f, indent=2)
        f.write("\n")

    print(f"Tagged {tagged} keys; Direct effect id {args.direct_id}; wrote {args.output}")
    if missing:
        print(f"No LED for matrix positions: {sorted(set(missing))}")
    if skipped:
        print(f"Skipped (non-default 'a' alignment), tag by hand: {sorted(set(skipped))}")


if __name__ == "__main__":
    main()
