# Configuration

sl_open stores its writable configuration as `config.cfg` in SDL_emfs's
user root. The retail installation remains read-only; the original
`starlancer.ini` is not modified.

The file is deliberately a small `name=value` format. Blank lines, comment
lines beginning with `#`, and unknown names are ignored. Missing or invalid
values keep the defaults.

## Paths

- Linux: the SDL preference path selected by SDL_emfs, normally below
  `$XDG_DATA_HOME/sl_open/sl_open` or the corresponding home data path.
- Windows: the SDL preference path selected by SDL_emfs below the user's
  application-data folder.
- Emscripten: the SDL preference path mounted and populated as IDBFS by
  SDL_emfs during filesystem initialization.

The retail asset root is the explicit `--data` argument, or the current
working directory when the argument is omitted. It is not written into the
user configuration because SDL_emfs creates one immutable asset-root context
for the application lifetime.

## Display

| Name | Range | Default |
|---|---:|---:|
| `display_width` | 640..16384 | 960 |
| `display_height` | 480..16384 | 720 |
| `display_mode` | 0 windowed, 1 borderless, 2 fullscreen | 0 |
| `vsync` | 0 or 1 | 1 |
| `brightness` | 50..200 | 100 |
| `texture_detail` | 0 low, 1 high | 1 |
| `graphics_detail` | 0 low, 1 medium, 2 high | 2 |
| `light_maps` | 0 or 1 | 1 |
| `default_view` | 0 cockpit, 1 chase, 2 no cockpit | 0 |
| `transitions` | 0 or 1 | 1 |
| `expand_widescreen_movies` | 0 or 1 | 1 |
| `pause_in_background` | 0 or 1 | 1 |

The frontend always uses its 640 by 480 logical canvas regardless of the
selected output resolution.

## Audio

| Name | Range | Default |
|---|---:|---:|
| `positional_audio` | 0 off, 1 standard, 2 HRTF | 1 |
| `effects_volume` | 0..127 | 80 |
| `music_volume` | 0..127 | 80 |
| `speech_volume` | 0..127 | 127 |
| `master_volume` | 0..127 | 127 |

HRTF remains selectable only when the active OpenAL device reports support.

## Controls

The controls section retains the 74 logical retail actions in their original
order. Physical keyboard locations are stored as SDL scancode numbers:

```text
binding_00=<scancode>,<modifier>,<joystick control>,<mouse control>
...
binding_73=<scancode>,<modifier>,<joystick control>,<mouse control>
```

Modifier values are 0 none, 1 Shift, 2 Control, and 3 Alt. A controller or
mouse control of `-1` means unassigned. Controller values 0..63 are buttons,
64..95 are the four directions of hats 1..8, and 96..97 are the left and
right gamepad triggers. Mouse values 1..32 are buttons and 33..36 are wheel
up, down, left, and right. Keyboard, controller, and mouse assignments are
independent, and assigning an already-used input moves it from the previous
action.

The top-level control values are:

| Name | Range | Default |
|---|---:|---:|
| `controller` | 0 joystick/gamepad, 1 mouse, 2 keyboard, 3 modern mouse | 0 |
| `force_feedback` | 0 or 1 | 1 |
| `invert_pitch` | 0 or 1 | 1 |
| `hat_control` | 0 or 1 | 1 |
| `twist_control` | 0 or 1 | 0 |

## Multiplayer

`multiplayer_address` stores the address entered in the multiplayer frontend.
Its default is empty.

## Campaign saves

Campaign profiles and saves use the same SDL_emfs user root as `config.cfg`.
Retail profiles and IFF saves are not read or modified.

`campaigns.idx` retains up to ten campaigns. Each campaign has a generated
128-bit hexadecimal ID used in `campaign-<id>.profile` and
`campaign-<id>-save-<slot>.sav`. Slot 0 is the automatic save; manual slots
are numbered 1 through 99.

Browser settings and saves persist in IndexedDB for the serving origin.
Changing the host, port, or scheme selects a different browser store.

The application now uses `sl_open/sl_open` as its preference namespace.
Settings and saves from builds using the previous `StarLancer/StarLancer`
namespace must be copied to the new user directory to reuse them.
