# Controls: input bindings file (GT4_CONTROLS_FILE)

Status: implemented for Windows (slice 179, TASK-029, release 1.0.0 workstream W3). The launcher
(W5) writes the file under `%LOCALAPPDATA%\GT4Recomp\controls.toml` and sets `GT4_CONTROLS_FILE` on
the game process. Unset means the built-in bindings, which are the same as before this change.

Labels: **Confirmed** (in code or tests), **Hypothesis** (not yet tested on hardware), **Unknown**.

**slice210 (TASK-075/076):** the pad backend is SDL3 3.4.18 (`GT4_INPUT=sdl`, the default; `GT4_INPUT=xinput` keeps the
XInput path for one release). The file format is unchanged: the `xinput:` source names name the physical positions
and drive every pad. The built-in keyboard keys changed to the TASK-076 layout (section `keyboard-default`). See
`docs/INPUT-SDL3.md`.

## What the file does

The PS2 pad has 16 digital inputs and 4 analog sticks. Each logical input lists the host sources
that drive it. A source is an XInput control or a keyboard key. Pressure-sensitive buttons
(the PS2 pressure set: D-pad, face, shoulder, L2 and R2) take their pressure from analog sources. Digital sources give full pressure (255).

- **Confirmed:** LT and RT drive L2 and R2 with analog pressure by default, as before. Each
  trigger sets the digital bit above its deadzone and the pressure from the value above it.
- **Confirmed:** keyboard keys are digital and cannot drive sticks.
- **Confirmed:** the defaults reproduce the slice 157 mapping bit for bit (checked over all 256
  trigger values and all 65,536 values of each stick axis, see Tests).

## Format

TOML subset, UTF-8. Comments start with `#` outside quotes. Strings use double quotes and cannot
contain `"`. No escapes, no multi-line values, no dotted keys, no inline tables.

```toml
schema = 1                     # required; must be exactly 1

[bindings]                     # the only table
cross = ["xinput:A", "key:Z"]  # one array of sources per input
l2 = ["xinput:LT | deadzone=0.05"]
rstick_y = []                  # an empty list unbinds the input
```

Rules:

1. `schema = 1` is required. Any other value is refused.
2. Only the `[bindings]` table exists. Any other table is refused.
3. Each key in `[bindings]` is an input name (table below). A name may appear once.
4. Inputs you list replace their defaults. Inputs you do not list keep their defaults. Use `[]`
   to unbind an input.
5. A value is a one-line array of quoted sources, or a single quoted source (`cross = "key:Z"`).
6. Duplicate sources on one input are refused.

Errors name the file and line: `C:\...\controls.toml:7: unknown XInput source 'Q'`. The game does
not start with an invalid file: the window stage stops with `[controls] invalid controls file,
game not started: ...` and exit code 1.

## Inputs

| Name | PS2 meaning | Kind | Default XInput | Default key |
| --- | --- | --- | --- | --- |
| `dpad_up`, `dpad_right`, `dpad_down`, `dpad_left` | D-pad | digital | DpadUp, DpadRight, DpadDown, DpadLeft | Up+E, Right+D, Down, Left+A |
| `cross`, `circle`, `square`, `triangle` | face buttons | digital, pressure | A, B, X, Y | W+Enter, Space, S, Backspace+Esc |
| `l1`, `r1` | shoulders | digital, pressure | LB, RB | Q, Shift |
| `l2`, `r2` | triggers | digital, pressure | LT, RT (analog) | F, R |
| `l3`, `r3` | stick clicks | digital | LThumb, RThumb | none |
| `select`, `start` | Select, Start | digital | Back, Start | C+Tab, P (start: hypothesis) |
| `rstick_x`, `rstick_y` | right stick | axis | RX, RY (invert=true on Y) | none |
| `lstick_x`, `lstick_y` | left stick | axis | LX, LY (invert=true on Y) | none |

The XInput names are positions (SDL 3.4.18, `docs/INPUT-SDL3.md`): `A` is the bottom face button (Cross on a
PlayStation pad), `B` the right, `X` the left, `Y` the top.

Stick and trigger sources are listed below. A stick input accepts only stick sources; a button
input accepts button, trigger and key sources.

## Sources

| Source | Kind | Drives |
| --- | --- | --- |
| `xinput:A`, `B`, `X`, `Y` | XInput face button | button inputs |
| `xinput:LB`, `RB` | shoulder button | button inputs |
| `xinput:Back`, `Start` | menu buttons | button inputs |
| `xinput:LThumb`, `RThumb` | stick click | button inputs |
| `xinput:DpadUp`, `DpadDown`, `DpadLeft`, `DpadRight` | D-pad | button inputs |
| `xinput:LT`, `xinput:RT` | trigger, 0..255 | button inputs, with pressure |
| `xinput:LX`, `LY`, `RX`, `RY` | stick axis, -32768..32767 | stick inputs only |
| `key:<name>` | keyboard key | button inputs only |

Key names (case-insensitive): `A`..`Z`, `0`..`9`, `F1`..`F12`, `Num0`..`Num9`, `Enter`,
`Backspace`, `Tab`, `Space`, `Shift`, `Ctrl`, `Alt`, `Esc`, `Up`, `Down`, `Left`, `Right`,
`Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`.

Reserved (refused in a file): `F7`, `F8`, `F9`, `F11`. The host uses them for the performance probe
and fullscreen. Alt+Enter toggles fullscreen; while Alt is held, Enter is not Start.

## Options

Options follow the source after a `|`, separated by spaces: `"xinput:LT | deadzone=0.05 curve=quadratic"`.

| Option | Applies to | Values | Default |
| --- | --- | --- | --- |
| `deadzone` | triggers, sticks | 0 up to, not including, 1; fraction of full travel | trigger 30/255 (0.1176); both sticks 3277/32767 (0.1) |
| `scale` | triggers, sticks | 0 to 2; multiplies the curve output, clamped to full travel | 1 |
| `curve` | triggers, sticks | `linear`, `quadratic`, `cubic`, `s_curve`, `smooth_power`, `sublinear`, `logarithmic` | `linear` |
| `invert` | sticks only | `true` or `false` | `false` |
| `game_deadzone` | sticks only | `skip` or `keep` | `skip` |
| `range` | triggers only | whole number 1 to 255; the pressure byte at full travel, on the game's own scale | 45 on R2, 84 on L2, 255 on any other input |
| `range_full` | triggers only | whole number 1 to 255; the same with the full trigger range on (TASK-161) | 255 |

Digital sources (`xinput:A` etc. and `key:` sources) take no options.

**Why R2 and L2 stop at 45 and 84 (TASK-147).** GT4 turns the pressure byte `p` of the button that drives a function
into `min(100 * min(2p, 256) / d, 256)`, with `d = 35` for the throttle and `65` for the brake (both games; v2.00
`0x0043BEE8` and `0x0043BF48`, Spec II `0x00319194` and `0x003191F0`). That is 100% throttle from `p = 45` and 100% brake
from `p = 84`, made for the DualShock 2's pressure buttons. A trigger that drove the whole byte reached 100% throttle at
about 27% of its travel and 100% brake at about 40%. R2 (the throttle in the usual trigger setup) and L2 (the brake) now
spread the game's own range over the whole travel. If the game's controller options put the brake on R2 or the throttle
on L2, set `range=84` on R2 or `range=45` on L2; `range=255` restores the whole byte.

**Full trigger range (TASK-161, experimental, off by default).** The launcher's "Full trigger range" toggle (Controls page,
above L2 and R2) passes `GT4_TRIGGER_FULL_RANGE=1` (`game.ini` `trigger_full_range=1` for a direct start). The game then
reads the throttle and the brake from the whole pressure byte, `min(100p / 99, 256)`, 100% from `p = 254`: about 255 levels
instead of 46 and 85 (`docs/research/TRIGGER-SENSITIVITY.md`). The same switch, read once by the game at start, sets the
triggers' default ranges: the whole byte (255) on every trigger with it on, 45 and 84 with it off, so the end of the travel is
100% either way and the launcher and a direct start cannot disagree. A binding's `range` applies only to the game's own scale
and `range_full` only to the full one: a `range=45` saved for TASK-147 never caps the throttle at 18% with the full range on,
and a `range_full` never changes the game's own scale. With the full range on, the brake on R2 or the throttle on L2 need no
option (both pedals use the same scale). The Controls page's trigger meters show the percentage of the game's 100% and the
pressure over that 100% (`28/45`, or `158/255` with the full range).

SDL3 Gamepad triggers already use 0 released through 32767 pressed for Xbox and DualSense alike.
The adapter reduces that standardized value to a pressure byte; raw joystick/HID signed ranges are
not read here. No device-name range override or `mode` source option is supported.

Reading rules, as implemented in `native/controls.cpp`:

- **Trigger:** raw value `v`, deadzone `d` (raw units, rounded). Digital bit set when `v > d`. With
  linear and scale 1: `pressure = (v - d) * 255 / (255 - d)`, integer division (the slice 157 formula).
  Otherwise `pressure = floor(min(1, curve(n) * scale) * 255)`, where `n = (v - d) / (255 - d)`. With a `range` `r` (or,
  with the full trigger range on, a `range_full` `r`)
  below 255: `pressure = round(min(1, curve(n) * scale) * r)`, to the nearest step so a trigger a hair short of its end
  still gives `r`.
- **Stick:** the sign of the raw value sets the travel range (32768 below zero, 32767 above), as
  before. `invert` flips the direction. Output is the PS2 byte: 128 at rest, 0..255 across travel. With
  `game_deadzone=keep` the byte is linear, `128 +- steps`. With `skip` (the default) a reading `steps` away from rest
  becomes `165 + round(90 steps / 127)` or `90 - round(90 steps / 128)`, past GT4's own stick dead zone (below).

**Why the sticks skip the game's dead zone (TASK-147).** GT4 maps a stick byte `b` to `(128b + 45) / 90` below 90, to 128
from 90 to 165, and to `128 + (127(b - 165) + 45) / 90` above 165 (both games; knot table at v2.00 `0x0064C4D8`, Spec II
`0x00681088`): a dead zone of 29% of each half, built for the DualShock 2. With the XInput dead zones it was stacked on
(24% left, 26.5% right), the first 46% or so of the travel did nothing and the rest steered 1.9 times too steeply. The
sticks now have a 10% dead zone, and past it the game steers in proportion to the travel, to the nearest of its own steps
(it has 90 bytes for 127 steps). The right stick on throttle or brake uses the same bytes.
- **Several sources on one input:** buttons OR together, and the highest pressure wins. On a stick,
  the reading furthest from rest wins.

## Example files

Shipped in `docs/controls/` and checked by `controls_fixture` (CTest `native_controls`) and
`test_controls_probe.py`.

- `default.toml`: the built-in mapping, written out in full. It parses to exactly the built-in map.
- `keyboard-only.toml`: all 16 buttons on the keyboard (WASD for the D-pad, J/K/H/U face buttons),
  sticks unbound. Use it when no controller is connected.
- `triggers-as-pedals.toml`: L2 and R2 from LT and RT with a 0.05 deadzone, everything else default.
  Use it with the in-game accelerate/brake assignment described in the owner test below.

## Launcher contract (W5)

- Write the file yourself; the game only reads it. Set `GT4_CONTROLS_FILE` to its absolute path on
  the game process. Do not set it in the system or user environment.
- Validate first. `scripts/external_runtime_probe.py` refuses a bad file before any run output
  exists (`--controls PATH`, or `controls_file=` in `play()`). The Python check covers the TOML
  shape, the schema, input names and source kinds. The C++ reader applies the full rules.
- `result.json` records the bindings in force:
  `"controls": {"source": "default"}` or
  `"controls": {"source": "file", "path": "...", "size_bytes": N, "sha256": "..."}`.
- An empty or unset `GT4_CONTROLS_FILE` means the built-in bindings. An inherited value is cleared
  for every probe run, so a stale variable cannot change the bindings.

### What the launcher's Controls page writes (TASK-019, 2026-10-21)

The format above is unchanged. The page writes the whole file after every change (no Save), always in
the order of the Inputs table, and keeps the old file when a change is refused or the write fails.

- **Rebind:** the new source replaces the button's source of the same family (`xinput:` or `key:`);
  the other family stays. A trigger that replaces a trigger keeps its options.
- **Conflict:** a source already on another button is not bound twice. Swap gives the other button
  the previous source of that family (or removes the source there when there was none).
- **Trigger rows:** `deadzone=` (0 to 0.95) and `curve=` on the `xinput:LT`/`xinput:RT` source of L2
  and R2.
- **Stick rows:** `deadzone=` (0 to 0.95) on every axis source of both axes of a stick; `invert` and
  the other options are kept. Example: `lstick_y = ["xinput:LY | invert=true deadzone=0.100"]`.
- **Reset all:** the built-in mapping, options included.
- A file written by a newer launcher (unknown schema) is kept as it is and the page is read-only.

## keyboard-default

The built-in keyboard keys (TASK-076, slice210). Hard-coded: they are the defaults of `native/controls.cpp`
(`default_controls`) and of `docs/controls/default.toml`, and the launcher shows them read-only. Keys are digital (full
pressure). F7, F8, F9 and F11 belong to the host and are not used.

```json
{
  "keyboard-default": {
    "dpad_up": ["Up", "E"],
    "dpad_right": ["D", "Right"],
    "dpad_down": ["Down"],
    "dpad_left": ["A", "Left"],
    "cross": ["W", "Enter"],
    "circle": ["Space"],
    "square": ["S"],
    "triangle": ["Backspace", "Esc"],
    "l1": ["Q"],
    "r1": ["Shift"],
    "l2": ["F"],
    "r2": ["R"],
    "l3": [],
    "r3": [],
    "select": ["C", "Tab"],
    "start": ["P"]
  }
}
```

Meaning: D-pad left and right also run on A and D; cross is accelerate and confirm; circle is handbrake; square brake;
triangle back; L1 look back; R1 nitro; L2 gear down; R2 gear up; select the camera. `start` = P is a hypothesis
pending the owner's choice (the table gives no Start key; Enter is Cross). Evidence and confidence: `docs/INPUT-SDL3.md`.

## Limits

- **Confirmed:** only the windowed path reads the file (`open_window`). A headless run (`--frames`
  without `--window`) never polls a controller; the file is not read there.
- **Confirmed (slice210):** SDL3 is the gamepad backend (`GT4_INPUT=sdl`, default; `xinput` keeps the XInput path for one
  release). DualShock 4, DualSense and Switch Pro pads are read through SDL's mapping; tested on the owner's pads only.
- **Confirmed:** no hot reload. Change the file, then start the game again.
- **Confirmed:** the rumble motors follow the same motor values on both backends and are not configurable here.
- **Hypothesis:** GT4 reads L2/R2 pressure and sticks only in the modes its menus choose. The file
  cannot change what the game does with a pressure or stick value it does not read. See the owner
  test below.
- **Unknown:** whether GT4 stores its own controller assignment on the memory card. This file
  does not change the card.

## Owner test (controller)

Run with the default file first, then with `triggers-as-pedals.toml`.

1. Start the window with no `GT4_CONTROLS_FILE`. Check that the log shows `[controls] default bindings`.
2. Move the sticks in the game menus. Check that the cursor follows the left stick and that the
   right stick Y reads as up/down in the same way as before (Y is inverted by default).
3. Press Cross, Circle, Square, Triangle, L1, R1, Start and Select. Each should register as before.
4. Hold LT and RT halfway and fully. Note whether the game reacts to pressure (accelerate or
   brake strength) after the in-game assignment (see step 6).
5. Start the probe with `triggers-as-pedals.toml` (`--controls` on `scripts\play_performance.py` or
   `GT4_CONTROLS_FILE` in the environment). Check that the log shows the file path and `result.json`
   records the sha256.
6. In the game's controller options, assign accelerate to R2 and brake to L2, as the owner did in
   PCSX2. Confirm throttle and brake respond in proportion to the trigger travel. Report what you see
   This is the open question the owner test answers; record the result in the journal.
7. Edit the file to break it (for example `cross = ["xinput:Q"]`). Start the window and check that
   the game does not start and the log shows the file, line and reason.

## Virtual controller layout (TASK-185, Android)

The phone's on-screen pad has its own file, **`user/virtual-pad.json`** (the `user` folder the launcher already uses for
`settings.toml` and the saves; on Android the app's own data folder, `/data/data/<package>/user`, which is the one the
launcher writes). The launcher edits it on the Controls page; the game reads it when it starts. It is a separate file
from `controls.toml` on purpose: controllers, wheels and the on-screen pad are three different things, and the pad layout
is shared by both agents through this one format.

The built-in layout is [`android/virtual-pad/default-layout.json`](../android/virtual-pad/default-layout.json); the
backend embeds the same bytes in the APK and the launcher compiles them in, so the two cannot drift. With no player file,
or one that cannot be used, the game uses the built-in layout and logs why.

### Mode (`settings.toml`)

`virtual_pad` is a launcher setting, `auto` (default), `always` or `off`:

- `auto`: the pad is shown only while no physical controller is connected;
- `always`: the pad is shown even with a physical controller;
- `off`: the pad is never shown.

It is stored in the `[settings]` table of `settings.toml` like every other launcher setting (the launcher has a single
settings table) and passed to the game at Play as **`GT4_VIRTUAL_PAD=auto|always|off`**. The launcher's own API for the
page is `get_virtual_pad` / `save_virtual_pad` / `reset_virtual_pad`; the row is drawn by the Controls page, not by
Settings > General.

### Format, version 1

Coordinates are fractions of the screen in landscape, so one layout fits any phone:

- `x` and `y` are the element's centre, 0..1;
- `size` is the element's height as a fraction of the screen height, 0..1;
- `width` is optional: the element's width as a fraction of the screen **width**. Without it the element is square
  (`size` on both axes). The default layout's L1/L2 and R1/R2 are 0.16 of the screen width, which is what puts the pair
  side by side.

```json
{
  "version": 1,
  "name": "GT4 default",
  "opacity": 0.6,
  "haptics": true,
  "elements": [
    { "id": "lstick",   "type": "stick",  "binds": "left_stick",  "x": 0.13, "y": 0.70, "size": 0.32 },
    { "id": "dpad",     "type": "dpad",   "binds": "dpad",        "x": 0.13, "y": 0.30, "size": 0.26 },
    { "id": "cross",    "type": "button", "binds": "cross",       "x": 0.88, "y": 0.78, "size": 0.15 },
    { "id": "square",   "type": "button", "binds": "square",      "x": 0.80, "y": 0.66, "size": 0.15 },
    { "id": "circle",   "type": "button", "binds": "circle",      "x": 0.96, "y": 0.66, "size": 0.15 },
    { "id": "triangle", "type": "button", "binds": "triangle",    "x": 0.88, "y": 0.54, "size": 0.15 },
    { "id": "l1",       "type": "button", "binds": "l1",          "x": 0.30, "y": 0.10, "size": 0.12, "width": 0.16 },
    { "id": "r1",       "type": "button", "binds": "r1",          "x": 0.70, "y": 0.10, "size": 0.12, "width": 0.16 },
    { "id": "l2",       "type": "button", "binds": "l2",          "x": 0.13, "y": 0.10, "size": 0.12, "width": 0.16 },
    { "id": "r2",       "type": "button", "binds": "r2",          "x": 0.87, "y": 0.10, "size": 0.12, "width": 0.16 },
    { "id": "select",   "type": "button", "binds": "select",      "x": 0.44, "y": 0.92, "size": 0.09, "width": 0.11 },
    { "id": "start",    "type": "button", "binds": "start",       "x": 0.56, "y": 0.92, "size": 0.09, "width": 0.11 }
  ]
}
```

Field rules:

- `type` is `button`, `dpad` or `stick`;
- a `button` binds one of `cross`, `circle`, `square`, `triangle`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `select`, `start`;
- a `dpad` binds `dpad`;
- a `stick` binds `left_stick` or `right_stick`, and may carry `deadzone` (0..0.5, default 0.08);
- each element may carry `"visible": false` and its own `opacity` (0..1) over the layout's;
- unknown fields are ignored (a later version of the format), an unknown `binds` drops that element with a log line.

Buttons are digital in version 1: full pressure (255) while held. A stick reports both axes, 0..255 with 128 at rest,
inside a circular range with the dead zone above; it is relative to where the thumb lands and recentres on release.

The file is replaced atomically (a temporary `virtual-pad.json.tmp` renamed over it), so a crash or a full disk leaves
either the old layout or the new one. A file whose `version` is above 1 is kept untouched (a newer launcher wrote it) and
the editor refuses to save over it.

### The F7 corner

The F7/F8 report button lives in the top-left corner of the game on Android (44dp at an 8dp margin, clear of the display
cutout and the system bars). The editor draws that corner as a reserved zone (5% of the width, 13% of the height) and
warns when an element overlaps it — more than a rounded edge on both axes. The warning does not block Save: the player may
prefer the button somewhere else, but the corner is meant to stay free.

### The editor (launcher)

Controls > Virtual controller > **Edit layout** opens a full-screen landscape editor over a dimmed race scene:

- the element list (one chip per button) and the canvas; a tap or Tab or the D-pad selects, A on a chip grabs the element;
- touch drag moves; a pinch or the corner handle resizes; the arrows (keyboard) or the left stick (pad) move the selected
  element; Shift+arrow or LB/RB resize; snap to grid (2%) is on by default;
- the selected element's size, width (wide buttons), opacity and shown/hidden; the layout's global opacity, vibration and
  snap; Undo (30 steps), Reset to default (loads the built-in layout into the editor), Save, Cancel (asks before losing
  changes);
- the 20:9, 16:9 and 21:9 previews reflow the same fractions; they never rewrite coordinates;
- elements are clamped inside the screen while they are moved or resized here. A layout the player already has is shown as
  it is: the editor never rewrites coordinates it was not asked to change.

### Checks

`cargo test --workspace` (the core's `virtual_pad` tests and the launcher's flow-audit test write and read a real file),
the focus checks in both languages (the section, the mode, the opacity, the whole editor with keyboard and pad,
the F7 warning, Undo, the previews, Save, Cancel and Reset) and nav-test (the pure layout rules: rectangles, clamping,
resizing, snapping, overlaps and the contract's validation).
