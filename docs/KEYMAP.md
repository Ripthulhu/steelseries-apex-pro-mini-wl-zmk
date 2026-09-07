# Keymap

Hold either Fn key to use the lower label on a key. The key where Caps Lock
normally sits is a second Fn key; this firmware does not assign Caps Lock.

![First-generation Apex Pro Mini Wireless keymap](keymap.svg)

## Fn bindings

- `Fn`+`Q` cycles the rapid-trigger reset distance, `Fn`+`T` toggles rapid
  trigger, and `Fn`+`I` / `Fn`+`O` move the actuation point.
- `Fn`+`E` toggles the lighting, `Fn`+`X` cycles the custom effects, `Fn`+`R`
  steps the effect, `Fn`+`Y` / `Fn`+`U` shift the hue, and `Fn`+`C` / `Fn`+`V`
  change brightness.
- `Fn`+`Z` toggles the optional
  [analog gamepad](FEATURES.md#analog-gamepad); `Fn`+`Right Ctrl`+`S`
  toggles Studio over USB. The two share one USB endpoint, so enabling either
  drops the other; Studio over Bluetooth is always available.
- `Fn`+`F`…`K` select Bluetooth profiles 0–4, `Fn`+`\` clears the current one.
- The bottom row (`Fn`+`B`/`N`/`M`/`,`/`.`/`/`) is media transport.
- To open the `APEXBOOT` update drive, hold Fn and press Esc and Right Ctrl
  together.

The full binding source is
[`apex_pro_mini_wl.keymap`](../apex-zmk-slot/boards/steelseries/apex_pro_mini_wl/apex_pro_mini_wl.keymap).

## Fn-layer lighting

While Fn is held the array stops showing the current effect and becomes a
legend, so the Fn functions and their state are readable at a glance. Releasing
Fn repaints the effect immediately.

### Mode toggles — green on, red off

Four keys are on/off modes. Held under Fn they show **green when on** and **red
when off**; toggling one also flashes it **green twice** when it turns on or
**red twice** when it turns off, even without Fn held.

| Key | Mode |
|---|---|
| `Z` | Analog gamepad |
| `S` | Studio over USB |
| `T` | Rapid trigger |
| `E` | Lighting on/off |

### Battery gauge — number row

The number row `1` … `=` is a 12-segment battery gauge. The **number of lit
keys is the charge level** (each key ≈ 8%), and the colour is the health:
**green above 40%, amber 15–40%, red below 15%**. The reading is the reported
state of charge, refreshed about once a minute.

### Category colours

The remaining Fn functions light by category:

| Colour | Function group | Keys |
|---|---|---|
| Orange | RGB hue / effect / brightness | `R` `Y` `U` `C` `V` |
| Amber | Actuation (reset, shallower, deeper, effect next) | `Q` `I` `O` `X` |
| Magenta | Media transport | `B` `N` `M` `,` `.` `/` |
| Blue | Bluetooth (profiles, clear) | `F` `G` `H` `J` `K` `\` |
| Cyan | Navigation and arrows | `W` `A` `D` `Del` `PrtSc` `Home` `PgUp` `Ins` `End` `PgDn` |
| Dim white | Esc (Fn = grave) | `Esc` |

`S` is the down-arrow under Fn, but its LED shows the Studio-over-USB state
rather than the navigation colour.
