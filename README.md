# StreamDeck

A DIY touchscreen macro pad built from an ESP32 and a 3.5" display. Tap a button on the screen and your PC does something: press a hotkey, skip a Spotify track, switch an OBS scene, open an app.

## How it works

The project has two halves:

```
┌──────────────────────┐   USB serial    ┌──────────────────────────┐
│  ESP32 + touchscreen │  ───────────▶   │  Python companion app    │
│  (firmware/)         │  "OBS:STREAM"   │  (companion-app/)        │
│  draws the buttons   │                 │  does the actual work    │
└──────────────────────┘                 └──────────────────────────┘
```

- **The ESP32** only draws the menus and listens for touches. When you press a button, it sends a short text command over USB, like `KEY:CTRL+C` or `SPOTIFY:NEXT`.
- **The companion app** runs on your PC, reads those commands, and carries them out: simulating key presses, talking to Spotify and OBS, or running a program.

Keeping the device "dumb" means all the clever stuff (logins, websockets, keyboard simulation) happens on the PC, where it's easy to change.

## Hardware

| Part | Notes |
|---|---|
| ESP32 dev board (classic ESP32) | No native USB needed, everything goes over serial |
| ILI9488 3.5" TFT, 480×320 | SPI display |
| XPT2046 resistive touch | Usually built into the display module |

### Wiring

| Display pin | ESP32 GPIO |
|---|---|
| MISO | 19 |
| MOSI | 23 |
| SCLK | 18 |
| CS | 5 |
| DC | 16 |
| RST | 17 |
| TOUCH_CS | 21 |

These are set in [firmware/platformio.ini](firmware/platformio.ini), so change them there if you wire it differently.

## Getting started

### 1. Flash the firmware

You'll need [PlatformIO](https://platformio.org/) (the VS Code extension is easiest).

1. Open the `firmware/` folder in PlatformIO.
2. Build and upload. PlatformIO downloads the libraries ([TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) and [LVGL](https://lvgl.io/)) for you.
3. On a classic ESP32 you may need to hold **BOOT** (and tap **RST**) when the upload starts.

> The touch calibration values in `main.cpp` (`calData`) belong to one specific panel. If your touches land in the wrong place, run TFT_eSPI's touch calibration example and paste in your own values.

### 2. Run the companion app

Requires **Windows** (the Spotify control uses Windows' media controls) and Python 3.

```bash
pip install pyserial pynput obsws-python winrt-Windows.Media.Control winrt-Windows.Foundation
```

Then, before the first run:

- In `companion-app/main.py`, set `comPort` to the port your ESP32 shows up on (check Device Manager).
- In `companion-app/obs_functions.py`, set the OBS websocket host, port and password to match **OBS → Tools → WebSocket Server Settings**.

Start it from the `companion-app/` folder:

```bash
python main.py
```

Each button press is printed in the terminal, which makes it easy to see what's happening.

## What the buttons can do

Every button sends one line in the form `CATEGORY:ACTION`. These are the categories the companion app understands:

### `KEY`: keyboard shortcuts

Simulates a key combination. Join keys with `+`; the last key is tapped while the others are held.

| Example | Does |
|---|---|
| `KEY:CTRL+C` | Copy |
| `KEY:WIN+SHIFT+S` | Screenshot tool |
| `KEY:MEDIA_PLAYPAUSE` | Media play/pause |

Named keys: `CTRL`, `SHIFT`, `ALT`, `WIN`, `MEDIA_PLAYPAUSE`, `MEDIA_NEXT`, `MEDIA_PREV`, `MEDIA_VOLUP`, `MEDIA_VOLDOWN`, `MEDIA_MUTE`. Any single character (`a`, `4`, `` ` ``) works as-is. Need another key? Add it to `KEY_NAMES` in `main.py`.

### `SPOTIFY`: music control

Talks to the Spotify desktop app directly through Windows. No Spotify login required.

| Action | Does |
|---|---|
| `SPOTIFY:PLAYPAUSE` | Play / pause |
| `SPOTIFY:NEXT` / `SPOTIFY:PREV` | Next / previous track |
| `SPOTIFY:SEEKFWD:10` | Jump forward 10 seconds (any number works) |
| `SPOTIFY:SEEKBACK:10` | Jump back 10 seconds |
| `SPOTIFY:SHUFFLE` | Toggle shuffle (or `SHUFFLE:ON` / `SHUFFLE:OFF`) |

### `OBS`: streaming and recording

Controls OBS Studio through its built-in websocket server. OBS must be running; if it isn't, the press is skipped with a message.

| Action | Does |
|---|---|
| `OBS:SCENE:<name>` | Switch to a scene |
| `OBS:STREAM` | Start / stop streaming |
| `OBS:RECORD` | Start / stop recording |
| `OBS:RECORDPAUSE` | Pause / resume recording |
| `OBS:MUTE:<source>` | Mute / unmute an audio source, e.g. `Mic/Aux` |
| `OBS:HIDETOGGLE:<source>` | Show / hide a source in the current scene, e.g. `Webcam` |
| `OBS:CLIP` | Save the replay buffer |

Names must match OBS exactly (including capitals). A few OBS settings to know about:
- `CLIP` only works while the replay buffer is **running**, not just enabled.
- `RECORDPAUSE` doesn't work when recording quality is set to "Same as stream".

### `SHELL`: run a command

Runs anything you could type in a command prompt, e.g. `SHELL:notepad` or `SHELL:code`.

## Changing the buttons

All pages and buttons are defined near the top of [firmware/src/main.cpp](firmware/src/main.cpp), as simple lists:

```cpp
static const ButtonDef keyButtons[] = {
  { LV_SYMBOL_COPY  "\nCopy",  "KEY:CTRL+C", 0 },
  { LV_SYMBOL_PASTE "\nPaste", "KEY:CTRL+V", 0 },
};
```

Each button has a **label** (an icon plus text), the **command** it sends, and a **target page**. Buttons that open another page set the command to `NULL` and use the target page instead; they show up orange.

To add a page:
1. Add a name to the `enum` of page indices.
2. Write its button list.
3. Add a row to `pages[]`, **in the same position** as in the enum.
4. Link to it from another page with a navigation button.

Each page fits up to 6 buttons (a 3×2 grid). Colors are all in one place under `// Colors` if you want to restyle it.

## Adding a new action

On the PC side, Spotify and OBS actions each live in a lookup table: `SPOTIFY_FUNCTIONS` in `spotify_functions.py` and `OBS_FUNCTIONS` in `obs_functions.py`. To add one, write a small function and add it to the table. The key you pick is the action name the button sends.

## Project layout

```
firmware/          ESP32 code (PlatformIO)
  src/main.cpp       pages, buttons, display + touch handling
  platformio.ini     board, libraries, pin setup
companion-app/     Python app that runs on the PC
  main.py            reads serial and routes commands
  spotify_functions.py
  obs_functions.py
docs/
  progress-log.md    detailed development notes
```

## Known limitations

- Windows only (Spotify control and the `SHELL` examples are Windows-specific).
- The COM port is hardcoded; unplugging the device stops the companion app.
- If OBS restarts while the companion app is running, restart the app too.
- Communication is one-way for now: the screen doesn't show what's playing or which scene is active yet.
