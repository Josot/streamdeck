# StreamDeck Project — Progress Log

**Purpose of this document:** full context dump for continuing this project in a future chat, and source material for a later portfolio item. Written in enough detail that someone (or an AI) with zero prior context could pick this up and continue.

---

## 1. Project Goal

Build a custom hardware macro pad / stream deck:
- Touchscreen display (ILI9488, 3.5", 320×480, resistive touch via XPT2046)
- Runs on an ESP32 microcontroller
- Controls **Spotify** playback and **OBS Studio** (scenes, mute, etc.)
- Generic **HID** (keyboard emulation) support for arbitrary hotkeys

---

## 2. Key Architecture Decision: Thin Client + PC Companion App

Two architectures were considered:

- **All-in-one on ESP32**: device does everything standalone (WiFi, HTTPS to Spotify, OBS websocket, HID, display). Rejected — too much to juggle on one memory-constrained chip, and Spotify's OAuth requires a browser redirect, which is awkward to self-host on a microcontroller (would need self-signed HTTPS certs etc.).
- **Thin client + companion app (CHOSEN)**: ESP32 only handles display, touch, and communicates over USB serial. A **Python companion app** running on the PC owns all the "smart" integrations:
  - `spotipy` for Spotify (handles OAuth via browser automatically, no manual token/cert handling)
  - `obsws-python` for OBS (local websocket, no cloud auth)
  - Can also simulate keystrokes directly (via `pyautogui`/`pynput`) for generic hotkey buttons — **this means the ESP32 does NOT need native USB HID at all.**

**This last point was a major realization mid-project**: since the PC app can simulate keypresses itself, the microcontroller's USB HID capability became irrelevant. This eliminated the need for a USB-OTG-capable chip (ESP32-S2/S3) and let us fall back to using the plain classic ESP32 we already had proven working, rather than debugging an unknown/possibly-dead S2 mini.

**Serial protocol (original plan, superseded — see v2 below):** simple line-based text over USB serial.
- ESP32 → PC: `BTN:7` (button press events)
- PC → ESP32: `TRACK:...`, `SCENE:...`, `MUTE:1` (status pushes, for showing now-playing track / highlighting active OBS scene / mic-mute indicator on screen)
- OBS pushes events instantly (websocket). Spotify has no push mechanism — PC app must poll `currently-playing` periodically (a few seconds' lag is fine and expected).

### 2.1 Serial protocol v2 (26-7-2026) — actions, not button numbers

Decided during the multi-page firmware build, superseding the `BTN:n` plan: buttons send **semantic action strings**, not positions. One line per press:

```
SPOTIFY:NEXT
KEY:CTRL+C
OBS:SCENE:1
DISCORD:MUTE
```

**Reasoning:** `BTN:3` becomes meaningless once buttons live on different pages of a multi-page UI. With action strings, the Python app just splits on `:` and dispatches — it never knows or cares about pages, grid positions, or layout. The UI can be rearranged freely without touching the Python side, and vice versa. Navigation presses (category buttons, back/prev/next) send nothing over serial; they're purely local.

**This is the contract the companion app builds against.** The PC→ESP32 direction (status pushes) is unchanged from the original plan and still not implemented.

### 2.2 UI architecture: data-driven pages (26-7-2026)

The multi-page UI is generated from `const` data tables, not written as code-per-screen — one generic `buildPage()` function turns a `PageDef` into a full LVGL screen. Rejected alternative: a hand-written create-function per screen (7+ near-identical functions, doesn't scale to the "infinite nested menus" target). Details of the structure live in section 8; the reasoning is: adding a page must be a data edit, not new layout code.

---

## 3. Hardware Inventory & Decisions

| Board | Native USB HID? | Status | Notes |
|---|---|---|---|
| Classic ESP32 (ESP32-D0WD-V3) | No (BLE HID only, no wired) | ✅ **Chosen board — fully working** | No native USB-OTG, only a CP2102 USB-to-serial bridge for flashing. Since HID isn't needed anymore (see above), this limitation stopped mattering. |
| ESP32-C6 | No (fixed-function USB Serial/JTAG only, hardware-locked) | Spare, unused | Confirmed via Espressif docs: cannot be reconfigured for HID under any circumstances. |
| ESP32-S2 mini (Lolin/Wemos) | Yes (full USB-OTG) | ⚠️ **Untested/unresolved** — never successfully powered on or enumerated, even after driver troubleshooting. Suspected dead solder joint (board was hand-soldered) or DOA. Not pursued further since HID need went away. Parked as a "maybe debug later" spare. |
| Raspberry Pi Pico W (RP2040) | Yes (native USB, HID-capable) | Spare, unused | Considered as alternative; ruled out mainly because it's a different ecosystem (not ESP32/Arduino), not because of a real technical blocker. |

**Display:** ILI9488, 3.5", 320×480, SPI, resistive touch via XPT2046 (bought from tinytronics.nl, 3.3V variant — no logic level shifter needed). Pin labels on physical board (hard to read silkscreen): GND, VCC, CLK, MOS, RRES, DC, BLK, MIS, CS1, CS2, PEN.

**Final wiring (classic ESP32, after fixing strapping-pin conflicts):**

| Display pin | ESP32 GPIO | Notes |
|---|---|---|
| GND | GND | |
| VCC | 3.3V | ⚠️ initially wired backwards with GND — no damage occurred, just no display, caught before any harm |
| CLK | 18 | SPI clock |
| MOS | 23 | MOSI |
| MIS | 19 | MISO |
| CS1 | **5** | Display chip-select — moved from original GPIO 15 (strapping pin) |
| DC | **16** | Data/command — moved from original GPIO 2 (strapping pin) |
| RRES | **17** | Reset — moved off original GPIO 4 mapping for tidiness/consistency with other control pins |
| BLK | 3.3V | Backlight, wired always-on for now |
| CS2 (touch) | 21 | Touch chip-select — shares CLK/MOSI/MISO bus with display |
| PEN (touch IRQ) | *not connected* | Intentionally skipped; polling touch works fine without it |

**Touch calibration data (specific to this physical panel):**
```cpp
uint16_t calData[5] = { 230, 3539, 255, 3493, 5 };
```

---

## 4. Major Hiccups & How They Were Solved

This section is important for the portfolio reflection — it documents real debugging, not just "everything worked."

### 4.1 CP2102 driver not installing automatically
- Classic ESP32 showed up in Device Manager under "Other devices" with a warning icon instead of under "Ports (COM & LPT)".
- Windows Update didn't have the driver. Had to manually download the Silicon Labs CP210x Universal Windows Driver, and install via **Device Manager → Update driver → Browse my computer → point at the `x64` folder** (the universal package has no standalone `.exe` installer, unlike some driver packages).

### 4.2 "Wrong boot mode detected (0x13)" / stuck at "Connecting......"
- First upload attempts failed because the board's auto-reset circuitry didn't reliably drop the chip into bootloader mode.
- **Fix discovered:** the BOOT/EN button press has to happen **while esptool is actively printing the connecting dots**, not before. Sequence: click Upload → wait for "Connecting......." to start → press+hold RST → press+hold BOOT → release RST → keep holding BOOT until dots stop/writing starts → release BOOT.
- This turned into a permanent manual step for every future upload on this specific board (no onboard auto-reset circuit, apparently). **Confirmed still true during LVGL bring-up (section 4.10+)** — this remains a normal, expected part of every upload on this board, not a regression or new problem.

### 4.3 GPIO strapping pins caused flash failures once display was wired
- Original pin plan put display DC on GPIO 2 and CS1 on GPIO 15 — both are ESP32 "strapping pins" that affect boot behavior.
- Symptom: `Warning: Failed to communicate with the flash chip` + `Serial data stream stopped: Possible serial noise or corruption` — but **only when the display was physically connected**. Uploading with the display fully unplugged worked cleanly, which was the diagnostic proof that pinned down the cause.
- **Fix:** moved DC → GPIO 16, CS1 → GPIO 5, RST → GPIO 17 (all clean, non-strapping, non-flash-bus pins). After this, uploads work fine with the display connected.

### 4.4 VCC/GND swapped during a rewiring pass
- After moving pins around, VCC and GND briefly got crossed. Result: nothing displayed, but no damage occurred. Caught by symptom (blank screen) before any harm; corrected by re-checking wiring against the pin table.
- **Lesson embedded in the process going forward:** always double check power pins specifically before powering on, since that's the one mistake that can actually damage hardware.

### 4.5 `LED_BUILTIN` not defined for generic "ESP32 Dev Module" board profile
- Stock Arduino "Blink" example failed to compile: `'LED_BUILTIN' was not declared in this scope`.
- Cause: the generic ESP32 Dev Module board profile in Arduino IDE doesn't define this macro (unlike named boards where it's mapped to a real pin).
- Fix: hardcode `#define LED_BUILTIN <pin>` and just guess/test pin numbers (tried 2, 5). Ultimately determined via testing that **this specific board has no onboard user-controllable LED at all** — the blinking seen during upload was just USB-serial TX/RX activity, not a GPIO LED. Not a bug, just a board without that feature.

### 4.6 ESP32-S2 mini never powered on
- Board is hand-soldered (headers added by hand). After acquiring a proper data-capable USB-C cable, plugging it in produced **zero LED activity and zero enumeration** in Device Manager.
- Ruled out cable/port as the cause (same cable/port confirmed working with the classic ESP32 immediately after).
- Visual inspection showed no obvious solder bridges or missing joints, but a cold/bad joint can look fine and still fail — root cause never conclusively identified. Likely dead board or subtle solder defect.
- **Decision:** shelved rather than debugged further, since it turned out not to be needed (HID requirement disappeared once the companion-app architecture's implications were fully worked through).

### 4.7 Compile times feeling too slow (~30s per iteration)
- Investigated via verbose compile output. **Conclusion: nothing was actually wrong** — the ESP32 core and all libraries (SPI, TFT_eSPI, FS, SPIFFS) were being cached correctly (`Using precompiled core`, `Using previously compiled file` confirmed in logs). The ~30s was legitimately the cost of: re-linking the final binary (unavoidable, single-threaded step) + esptool's connect/handshake dance with the board (made slower by the manual boot-button timing from 4.2) + flashing.
- Practical mitigation: use **Verify** instead of **Upload** while just checking code correctness, since it skips the connect/flash phase entirely.
- This investigation was also the direct motivator for switching to PlatformIO (see below) — even though the cache turned out to be fine, the exercise surfaced other reasons to switch (see section 5).

### 4.8 Switch to PlatformIO — several setup hiccups
- **Reason for switching:** Arduino IDE's rigid single-folder-per-sketch structure doesn't play well with Git/version control (can't easily organize firmware + companion app together), and PlatformIO offers proper project structure, per-project config files, and native VS Code Git integration.
- **Hiccup A:** First "New Project" attempt appeared to hang indefinitely on "Please wait... first initialization requires internet connection." Diagnosed as a genuine first-run toolchain download (a few hundred MB), not a real hang — confirmed by checking the PlatformIO Core CLI log/progress bar rather than the static dialog text. (One instance of this seemed to hang far longer than normal with no clear cause, then suddenly resolved itself on retry — never fully explained, possibly a transient network/server issue.)
- **Hiccup B:** After moving the generated project folder into a `firmware/` subfolder (for the repo structure, see section 6), VS Code's terminal/PlatformIO kept referencing the old path (`...\Desktop\streamdeck` did not exist). Fixed by using **File → Open Folder** to explicitly re-point VS Code at the folder that directly contains `platformio.ini` (one level deeper than expected, inside `firmware/`).
- **Hiccup C:** GitHub Desktop's "Create a new repository" dialog was mistakenly used instead of "Add local repository" — the former creates a brand-new empty folder rather than adopting existing files, causing confusion (folder name collisions, "thingy" test folder, etc.). Resolved by doing `git init` manually via command line directly in the existing project folder instead of fighting the GUI.
- **Hiccup D:** `.gitignore` file created via `notepad .gitignore` appeared to be "missing" — actually just hidden by Windows Explorer (dot-prefixed files are hidden by default). Resolved via View → Hidden items, or `dir /a`.

### 4.9 TFT_eSPI config approach differs between Arduino IDE and PlatformIO
- Arduino IDE approach: hand-edit `User_Setup.h` inside the library's installed folder.
- PlatformIO approach (cleaner, used going forward): pass the exact same settings as `-D` build flags inside `platformio.ini`, with `-DUSER_SETUP_LOADED=1` telling TFT_eSPI to skip looking for `User_Setup.h` entirely. This keeps all config version-controlled and project-local instead of buried in a global library folder.

### 4.10 LVGL added — dependency install is silent/automatic, not a manual trigger
- Added `lvgl/lvgl @ ^9.2` to `lib_deps` in `platformio.ini`, expecting to need to manually trigger a build to see it install.
- **Clarified misunderstanding:** saving `platformio.ini` in VS Code automatically triggers PlatformIO's dependency resolution in the background — no popup, no manual build needed for *that* step. The resulting install log (`Library Manager: lvgl@9.5.0 has been installed!`) appears in the Output/Terminal panel, not a dialog.
- Note: this dependency-resolution step is separate from an actual **Build**, which still must be triggered manually (checkmark icon / `pio run`) and which actually compiles code.

### 4.11 `lv_conf.h` needed, but didn't error out until `lvgl.h` was actually included
- Initially expected an immediate error about missing `lv_conf.h` right after adding the LVGL dependency — didn't happen, because nothing in `main.cpp` was including `<lvgl.h>` yet, so the compiler had no reason to look for LVGL's config at all.
- **Fix:** copied `firmware/.pio/libdeps/esp32dev/lvgl/lv_conf_template.h` → `firmware/include/lv_conf.h`, changed the top-of-file `#if 0` to `#if 1` (this is the master switch enabling the whole rest of the template — it ships disabled by default), confirmed `LV_COLOR_DEPTH` was `16` (matches how TFT_eSPI talks to the ILI9488).
- Added `-DLV_CONF_INCLUDE_SIMPLE=1` to `build_flags` in `platformio.ini` so LVGL looks for `lv_conf.h` in the project's own `include/` folder instead of demanding edits inside `.pio/libdeps` — same reasoning as the TFT_eSPI build-flag approach in 4.9 (keep config project-local and git-tracked).
- After this, adding a bare `#include <lvgl.h>` to `main.cpp` compiled successfully — confirmed the config wiring works before writing any actual LVGL calls.

### 4.12 DRAM overflow linker error when wiring up display flush + touch read callbacks
- After adding `my_disp_flush`/`my_touch_read` callbacks, `lv_display_create`/`lv_indev_create` setup, and a `buf1` display buffer sized `screenWidth * 40` (~37.5KB), build failed at link time:
  ```
  section `.dram0.bss' will not fit in region `dram0_0_seg'
  region `dram0_0_seg' overflowed by 1744 bytes
  ```
- **Diagnosis:** `.dram0_0_seg` is the ESP32's static-RAM region (320KB total budget). Two things were competing for it: `buf1` (~37.5KB) and LVGL's internal object/style memory pool (`LV_MEM_SIZE` in `lv_conf.h`, generous by default in the template since it doesn't know this chip's real budget), on top of what the Arduino/WiFi/BT framework itself already reserves.
- **Fix applied:** shrank `buf1` from `screenWidth * 40` to `screenWidth * 20` (~19KB) — halves the flush-buffer footprint at the cost of more (smaller) flush cycles, not a UI complexity limit.
- **Deliberately did NOT touch `LV_MEM_SIZE`:** that pool is what every future widget (buttons, labels, menus) actually allocates out of *at runtime*, so shrinking it would create a ceiling on future UI complexity — unlike `buf1`, which is a fixed-size scratch buffer LVGL reuses regardless of how many widgets exist. `LV_MEM_SIZE` is reserved as the fix for a *different* future symptom (`lv_mem` allocation warnings at runtime, not a link-time DRAM error) if that ever comes up.
- After the `buf1` fix: build succeeded, RAM usage reported as 32.7% (107120 / 327680 bytes) — comfortable headroom.

### 4.13 COM port busy on next upload attempt
- `A fatal error occurred: Could not open COM13, the port is busy or doesn't exist.` / `PermissionError(13, 'Access is denied.')`
- **Cause:** a serial monitor session from the previous `--target upload --target monitor` run was still holding COM13 open in a VS Code terminal tab.
- **Fix:** closed the leftover monitor terminal (Ctrl+C / close tab), retried upload — worked immediately. Noted as a recurring class of issue to check first whenever a COM port "goes missing" after a monitor session.

### 4.14 First LVGL screen render was blank/white — expected, not a bug
- After the DRAM fix, first successful flash showed a plain white screen with backlight on, no visible content.
- **Correctly self-diagnosed in session:** LVGL's default screen background is white, and no widgets had been created yet — a white screen with backlight on is exactly the expected state before any `lv_obj` is created, not a display/wiring fault.

### 4.15 Runtime style changes (from `loop()`) silently didn't redraw — real LVGL bug, methodically isolated
- While experimenting with a touch-triggered background color change (`lv_obj_set_style_bg_color(lv_screen_active(), ..., 0)` called from inside `loop()`), the color visually never changed, despite the surrounding code running correctly (confirmed via `Serial.println` firing exactly as expected).
- **False leads ruled out first, in order, each with a real test:**
  - Suspected the artificial `delay(100)` test slowdown (added for an earlier, unrelated "watch LVGL redraw in visible slices" experiment) was causing a watchdog reset — ruled out by re-testing the known-good button-only version with the same delay still present; it rendered fine.
  - Suspected the `static bool toggled` one-shot guard was somehow blocking rendering — logically ruled out (a bool that's only read *after* a touch cannot affect boot-time rendering) and empirically ruled out (re-flashing the identical "step 1" print-only version, then the `toggled` version, both worked — the one earlier "nothing rendered" run was a one-off, most likely a marginal jumper-wire contact given this board's wiring history, not a code fault).
  - Discovered mid-investigation that a test string ("changing color") was printing without actually being paired with the real `lv_obj_set_style_bg_color` call — an artifact of incrementally stripping code down during isolation, not a bug, but a good reminder to check what a diagnostic print is actually next to, not just its text.
  - Found and fixed a **real, separate bug** in one intermediate version: `lv_button_create(...)` had accidentally ended up called from inside `loop()` instead of `setup()`, recreating a brand-new button object on every single loop iteration (hundreds of times per second) — masking any color change since a fresh orange button was redrawn on top immediately after. Fixed by promoting `lv_obj_t *btn` to a global and creating it exactly once, in `setup()`, per the same pattern already used for other cross-function variables (`size`, `gap`, etc., see section 8).
- **After that fix, the color change still silently failed to redraw** — isolated further by adding a `Serial.printf` inside `my_disp_flush` itself, confirming the full 16-slice boot-time render produced flush calls as expected, but **zero new flush calls occurred after the touch-triggered style change**, even though the style-setter call itself definitely ran.
- Tried `lv_obj_invalidate(lv_screen_active())` after the style change as a forced "mark this dirty" call — still no effect, no new flushes.
- **Actual fix:** added `lv_refr_now(disp)` immediately after the style change, which forces LVGL to run an immediate synchronous refresh cycle rather than waiting for its normal periodic scheduling (driven by repeated `lv_timer_handler()` calls in `loop()`). This **did** trigger new flushes and the background genuinely changed color. Required promoting `lv_display_t *disp` to a global (same pattern as `btn`) so `loop()` could reach it.
- **Follow-up test showed `lv_obj_invalidate` was unnecessary** — `lv_refr_now(disp)` alone, with no explicit invalidate call, was sufficient to trigger the redraw. Root cause of *why* the normal periodic refresh doesn't pick up runtime style changes reliably on its own was not conclusively identified (possibly an LVGL 9.5-specific scheduling quirk, possibly an interaction with the tight `lv_timer_handler(); delay(5);` loop structure) — not resolved, just reliably worked around.
- **Established going-forward rule:** any style/property change made from `loop()` (i.e. after initial setup-time creation) should be followed by `lv_refr_now(disp)` to guarantee the change is actually visible immediately, rather than assuming LVGL's automatic refresh will pick it up on its own schedule.
- Diagnostic scaffolding (delay, heartbeat prints, flush-logging, pointer-comparison prints) was stripped back out afterward, keeping only the `disp`/`btn` global promotions and a short code comment flagging the `lv_refr_now` requirement for future reference.

> **Addendum 26-7-2026 — ROOT CAUSE FOUND, see 4.16.** The mystery is solved: LVGL's tick counter was never being advanced (`lv_tick_inc()` was missing entirely), so LVGL's periodic refresh timer never fired — "possibly a scheduling quirk" was in fact a starved clock. The `lv_refr_now()` workaround treated the symptom. It is now **obsolete and removed from the code**; the going-forward rule above no longer applies. Runtime style changes render on their own since the tick fix (verified on hardware via pressed-state button colors).

### 4.16 Touch completely dead on first multi-page build — LVGL's clock was never running (26-7-2026)
- **Symptom:** new multi-page firmware (see section 8) flashed cleanly and the UI drew correctly, but touching category buttons did nothing. No navigation at all.
- **Key realization before touching anything:** LVGL's touch pipeline had *never actually been tested*. In the old single-button code, the visible on-touch color change came from the manual `tft.getTouch()` block in `loop()` — not from LVGL. `my_touch_read` was registered but nothing had ever proven LVGL called it. So this wasn't "touch broke" — it was the LVGL input path failing its very first real test.
- **Method:** same bisect-with-instrumentation approach as 4.15. Added a once-per-second heartbeat print inside `my_touch_read` plus a coordinate print on detected touches, with three possible outcomes mapped in advance to three different culprits (LVGL never polls / `getTouch` returns nothing / coordinates rotated vs. calibration).
- **Result: total silence.** Not even the heartbeat printed. LVGL never called the read callback once.
- **Root cause:** LVGL does not track time by itself on Arduino. Everything periodic inside it (input polling, screen refresh, animations) runs on internal timers that measure elapsed time via a tick counter — and that counter only advances when the application calls `lv_tick_inc()`. Nothing in the code ever did. Every LVGL timer saw "0 ms have passed" forever and never fired. The UI still drew at boot because the first render doesn't depend on those timers; everything periodic after that was dead.
- **Fix:** one line in `loop()`:
  ```cpp
  void loop() {
    lv_timer_handler();
    delay(5);
    lv_tick_inc(5);  // advance LVGL's clock by the 5ms we just slept
  }
  ```
- **This retroactively solved 4.15** (see addendum there): "periodic refresh doesn't pick up runtime style changes" is exactly the fingerprint of a refresh timer that never fires because time never advances. All `lv_refr_now()` workarounds were subsequently removed; pressed-state button colors (runtime style changes handled entirely by LVGL) confirmed rendering on their own on hardware.
- **Lesson:** a callback being *registered* proves nothing about it being *called*. First test for any callback: print inside it and confirm the print appears at all, before debugging anything downstream of it.

### 4.17 Build failed: `firmware.bin` locked by another process (26-7-2026)
- `The process cannot access the file because it is being used by another process` on `.pio\build\esp32dev\firmware.bin`, at the build stage (before upload/COM was ever involved).
- Likely cause: the project lives under `Documents\` (OneDrive-synced on this machine); OneDrive grabs freshly written files for upload and can hold a lock on the constantly rewritten `firmware.bin` at exactly the wrong moment. (Antivirus real-time scanning is the other usual suspect.)
- **Fix used:** plain retry — these locks are transient, and the second attempt succeeded.
- **Durable options noted for if it recurs:** move the project outside any synced folder, or relocate just the build output via `build_dir = <path outside OneDrive>` under `[platformio]` in `platformio.ini` (source stays synced/backed up, churning binaries don't).

---

## 5. Toolchain Migration: Arduino IDE → PlatformIO

**Reasons for switching (came up organically over the course of the project):**
1. Arduino IDE's sketch-folder rules made it awkward to keep firmware + companion app + git config together in one coherent project.
2. Wanted proper version control from early on (see section 6) — PlatformIO's project format (a real folder with `platformio.ini`, `src/`, `lib/`, `include/`) is much more Git-friendly than Arduino IDE's rigid single-`.ino`-plus-tabs model.
3. Investigating compile speed (section 4.7) surfaced these structural frustrations even though the actual caching turned out to be fine.

**Setup used:** VS Code + PlatformIO extension. Board: `esp32dev` (Espressif ESP32 Dev Module). Framework: **Arduino** (deliberately, not ESP-IDF) — reasoning: all existing code and libraries (TFT_eSPI, LVGL) are written for the Arduino framework/API; ESP-IDF would require rewriting everything against lower-level APIs for no benefit in this project, since Arduino-on-ESP32 is itself built on top of ESP-IDF anyway (not a "lesser" option, just a friendlier layer).

**`platformio.ini` used (working, current state):**
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200

lib_deps =
    bodmer/TFT_eSPI @ ^2.5.43
    lvgl/lvgl @ ^9.2

build_flags =
    -DUSER_SETUP_LOADED=1
    -DILI9488_DRIVER=1
    -DTFT_MISO=19
    -DTFT_MOSI=23
    -DTFT_SCLK=18
    -DTFT_CS=5
    -DTFT_DC=16
    -DTFT_RST=17
    -DTOUCH_CS=21
    -DSPI_FREQUENCY=27000000
    -DSPI_READ_FREQUENCY=20000000
    -DSPI_TOUCH_FREQUENCY=2500000
    -DLV_CONF_INCLUDE_SIMPLE=1
```

Installed LVGL version resolved to **9.5.0** (from the `^9.2` range).

**Serial monitor in PlatformIO:** plug icon 🔌 in the bottom VS Code toolbar, or `pio device monitor` in terminal. Respects `monitor_speed` from the ini file automatically. **Note:** leaving a monitor session open blocks the next upload attempt on that COM port (see 4.13) — close it before re-uploading.

---

## 6. Project & Repo Structure

Decided on a monorepo with firmware and companion app as clean sibling folders, one Git repo covering both:

```
<repo root>/
├── .git/
├── .gitignore              (covers both subfolders — .pio/, __pycache__/, .venv/, OS junk)
├── firmware/                (PlatformIO project — open THIS folder in VS Code for firmware work)
│   ├── platformio.ini
│   ├── platformio.ini's own .gitignore (auto-generated by PlatformIO, harmless overlap with root one)
│   ├── src/
│   │   └── main.cpp
│   ├── include/
│   │   └── lv_conf.h        (LVGL config, copied from template — see section 4.11)
│   └── lib/
└── companion-app/            (Python companion app — not started yet)
```

- Git set up via command line (`git init`, manual `.gitignore` creation via Notepad, `git add .` + `git commit`) after GitHub Desktop's dialogs caused confusion.
- Repo has since been published to GitHub via GitHub Desktop's "Publish repository" button.
- Note: PlatformIO auto-generates its own `.gitignore` inside `firmware/` — this coexists harmlessly with the root-level one (Git `.gitignore` files stack; redundancy across the two is not a conflict).

---

## 7. Roadmap & Current Progress

| # | Step | Status |
|---|---|---|
| 1 | Display bring-up (draw shapes/text, no touch) | ✅ Done |
| 2 | Touch bring-up + calibration | ✅ Done (calData captured above) |
| 3a | **Manual** button grid + manual coordinate hit-testing (deliberately done before LVGL, for learning) | ✅ Done |
| 3b | Generalize manual hit-test to all buttons in the grid (loop over all rows/cols) | ✅ Done — `whichButton()` generalized with nested `for(currentRow)/for(currentColumn)` loop, prints `BTN:1`–`BTN:6` using formula `currentRow * maxColumns + currentColumn + 1` |
| 3c | LVGL setup (replaces manual drawing/hit-testing with a real UI library) | ✅ Done — library installed, config wired, display flush + touch read callbacks working, DRAM overflow fixed, single button + label rendering and confirmed on screen |
| 3d | LVGL: generalize to full 6-button grid using LVGL widgets | ✅ Done (26-7-2026) — went straight to the full data-driven multi-page version, skipping the intermediate single-page 6-button step |
| 3e | LVGL: styling (background/button colors) | ✅ Done (26-7-2026) — color constants centralized, pressed-state feedback via `LV_STATE_PRESSED` styles confirmed on hardware (small touch-to-highlight delay, within acceptable range for resistive touch + LVGL's polling interval). `lv_refr_now` rule obsolete per 4.16. |
| 3f | LVGL: multi-page navigation (home → category pages → paged categories) | ✅ Done (26-7-2026) — 9 pages working on hardware incl. a 3-page Spotify chain; navigation tested up/down/sideways. Spotify page 3 content is placeholder. |
| 4 | USB HID | ❌ **Dropped as a requirement** — companion app handles keystroke simulation instead (see section 2) |
| 5 | Wire button grid → serial | 🟡 ESP32 side done under **protocol v2** (`CATEGORY:ACTION` strings, see 2.1) — action buttons call `Serial.println(def->action)` on click, fully replacing the manual `whichButton()` polling (which is now deleted). **Explicit monitor verification of the output lines still pending** (navigation + highlights are hardware-verified; the serial lines themselves haven't been checked off). PC side not started. |
| 6 | Two-way serial protocol (status pushes back to display: track info, scene state, mute state) | ⬜ Not started |
| 7 | Python companion app — Spotify (`spotipy`) | ⬜ Not started |
| 8 | Python companion app — OBS (`obsws-python`) | ⬜ Not started |
| 9 | Polish (icons, config format, case, etc.) | ⬜ Not started — roadmap idea added 26-7-2026: long-term, a layout editor in the companion app (device stores a layout pushed from the PC) so button changes never require compiling firmware; see section 12 item 6 |

**Toolchain/infra status (all done, not part of the numbered roadmap but consumed real time):**
- ✅ Arduino IDE toolchain fully working on classic ESP32 (superseded by PlatformIO but was the original proof-of-concept environment)
- ✅ Switched to PlatformIO + VS Code, fully working build/upload/monitor cycle
- ✅ Git version control set up (command line), repo published to GitHub
- ✅ LVGL 9.5.0 installed and wired to TFT_eSPI (flush + touch read callbacks), DRAM budget fixed
- ✅ `lv_tick_inc()` wired in `loop()` — mandatory for LVGL timers on Arduino (see 4.16)

---

## 8. Current Working Code (as of this log)

**Full current firmware: see git page, date 26-7-2026** (`firmware/src/main.cpp`). Older versions (manual hit-test version, single-button LVGL bring-up) live in git history — no longer embedded here.

### Architecture of the current firmware (multi-page, data-driven)

All UI content lives in `const` data tables at the top of the file; one generic `buildPage()` generates every screen from them.

- **`ButtonDef`** — one button: label (LVGL symbol + `"\n"` + text glued as adjacent string literals), serial action string, target page. Convention: `action == NULL` means "navigation button, use `targetPage`"; otherwise "action button, send `action` over serial on click". NULL as sentinel — one field, two meanings.
- **`PageDef`** — one screen: title, pointer to its button array, count, and three navigation links: `parentPage` (back button → home), `prevPage`/`nextPage` (sibling pages within a category). `-1` = link absent = that top-bar button isn't created for this page.
- **`pages[]`** + a matching **enum** of page indices — the master table. ⚠️ The enum and the array are only connected by *order*; see the checklist below.
- **Layout:** 50px top bar (back button left at x=8, title centered, prev/next arrows right at x=344/x=412, all 60×40) + 3×2 grid below (140×115 buttons, `start + index*stride` with `xStart=15, xStride=155, yStart=63, yStride=128`; row/col recovered from flat index via `i / gridCols` and `i % gridCols`). Prev/next live in the top bar deliberately, so all 6 grid slots stay available for real actions.
- **Events:** one shared `actionButtonEvent` for all action buttons and small per-role callbacks for back/prev/next, each receiving its identity via LVGL's `user_data` pointer (the button's own `ButtonDef` row, or the page's `PageDef`). See section 11 for the concept.
- **Press feedback:** second background color bound to `LV_STATE_PRESSED` per button — LVGL swaps it on touch-down/up itself, zero event code.
- **Screens:** all built once at boot into a `screens[]` array; page switches are just `lv_screen_load()`. Fine at this scale (~9 pages); revisit only if the tree grows huge.
- **Colors** centralized as named constants (`COL_ACTION`, `COL_NAV`, pressed variants, etc.) — retheming the device is a six-line edit.
- Current page tree: Home → {Keybinds, Discord, Spotify 1/2/3 (chained), OBS, Media, System}.

### How to add a page (validated 26-7-2026 by adding Spotify 3/3 independently)

Four touches, all in the data section, zero logic changes:

1. **Button array** — define `xyzButtons[]`, up to 6 entries.
2. **Enum** — add `PAGE_XYZ` *in the same position* the page will have in `pages[]`.
3. **`pages[]` row** — insert the entry at that same position.
4. **Re-link** — if it extends a category chain: previous page's `nextPage` → new page, new page's `prevPage` → back. Update the "n/m" counts in the title strings of every page in the chain (the counts are plain data in the titles, nothing computes them).

**THE trap:** enum and `pages[]` are connected only by order. Adding the enum entry in one place and the array row in another silently shifts every later page index onto the wrong screen. Insert both in the same spot, always.

Convention decided: back from any page in a chain goes straight to home (`parentPage = PAGE_HOME` for all of them); prev/next handle movement within the chain, so back never duplicates prev.

Known placeholder: Spotify 3/3's labels/icons are copy-paste artifacts that don't match the actions its buttons actually send (e.g. a "Queue"-labelled button sends `SPOTIFY:SEEKFWD`). Harmless as a navigation test, must be filled in or parked before relying on it.

---

## 9. Learning Approach / Pedagogical Notes (relevant for portfolio reflection)

This project is explicitly being used as a hands-on learning exercise (student studying Technische Informatica), not just "get it working." Notable learning-focused choices made along the way:

- Deliberately building the **manual** version of drawing/grid/touch-hit-testing before introducing LVGL, so the concepts LVGL automates (coordinate math, hit-testing) are understood first-hand rather than taken as a black box.
- Started with small, incremental drawing exercises (color screen, single square, centered text, stacked rectangles, a labeled button, a bullseye, a 2×2 grid via nested loops) before attempting the full grid — each exercise targeted one specific concept (coordinate system, draw-order/layering, spacing formulas, etc.).
- The `start + index * stride` formula (used for grid positioning) was explicitly worked through line-by-line as the core reusable spatial-math concept, then **successfully reused unprompted** to generalize `whichButton()` from a single hardcoded check into the full 6-button loop (3b) — a concrete sign the concept actually transferred, not just copy-pasted.
- The button-numbering formula (`currentRow * maxColumns + currentColumn + 1`) was worked out by the student with a concrete example (row 1, col 2) rather than derived abstractly first.
- When debugging real code the student wrote (not code I wrote for them), bugs were explained conceptually (e.g., "off-screen rectangle", "code outside a function", "missing semicolon", "typo in a type name") rather than just silently fixed — this was an explicit ask from the student partway through ("stop suggesting your code... we're using this code").
- Hardware debugging (driver installation, boot-mode button timing, strapping-pin conflicts, VCC/GND swap) was treated as genuine, valuable troubleshooting experience worth documenting, not just an obstacle to route around.
- **LVGL introduction was handled differently on purpose:** unlike the manual grid/touch-hit-test work, LVGL's display/touch plumbing (`lv_display_create`, flush/read callbacks, buffer setup) is boilerplate API wiring with no real "figure it out yourself" value — it was given directly, with explanation of *what each call does and why*, rather than scaffolded as a discovery exercise. Widget usage (buttons/labels) started with fuller explanation too once it became clear the API syntax itself (pointers, parent/child object trees, create/configure pattern) was the actual sticking point, not the logic — the student explicitly asked for the code directly partway through this section ("just give it and explain it to me"), and separately asked for a plain-language explanation of C pointer syntax (`lv_obj_t *`) since that concept was new and unclear.
- The student **independently and correctly diagnosed** a blank/white LVGL screen as expected default behavior (no widgets created yet) rather than assuming something was broken — a good sign of the mental model forming correctly.
- The student also correctly linked a COM-port-busy error to a leftover serial monitor session before being told the cause, applying troubleshooting instincts from earlier hardware/tooling debugging (section 4) to a new tooling issue.
- Asked a good clarifying question about whether shrinking LVGL's memory (to fix the DRAM overflow) would limit future UI complexity — showed active tracking of downstream consequences of a proposed fix rather than just accepting it, and prompted a clear distinction between "safe to shrink" (a reusable buffer) vs. "will bite you later" (the runtime widget memory pool).
- Explicitly paused mid-implementation to ask "is LVGL really the best way to do this?" rather than just continuing on inertia once real friction (memory tuning, new syntax) showed up — a good habit of periodically re-justifying a tool choice against the actual project scope rather than sunk-cost continuing. Confirmed LVGL was the right call once the actual scope ("infinite" nested menus) was stated explicitly, since that's precisely the class of problem LVGL's screen/object model solves and hand-rolling would solve badly.
- The section 4.15 refresh-bug investigation was a genuine, non-trivial debugging session (not a simple typo) — worth highlighting for the portfolio reflection specifically because the student pushed back on a plausible-but-wrong first fix (rejecting "it just glitched" as an explanation) and insisted on isolating the real cause via a structured process: bisecting between known-working and known-broken versions, adding targeted instrumentation (heartbeat prints, flush-call logging, pointer comparisons) rather than guessing repeatedly, and correctly noticing when a diagnostic print's *text* had drifted from what the code *actually* did after incremental edits. This is a strong, concrete example of real debugging methodology for the eventual portfolio reflection, distinct from hardware debugging (section 4) — this time entirely in software/library behavior.

**Added 26-7-2026 (multi-page prototype session):**

- **Mode shift, explicitly chosen:** this session ran in deliberate prototype-rush mode ("need a working prototype to start the Python application") — the multi-page firmware was written by the AI at the student's explicit request, consistent with the established LVGL-boilerplate precedent, with every new C concept annotated in-chat rather than scaffolded as discovery exercises. A plan was still made and reviewed *before* any code was written (page tree, layout, protocol, event model), at the student's insistence ("plan first").
- The 4.16 tick investigation completes the 4.15 story arc for the portfolio: the same structured instrumentation method (heartbeat print, outcomes mapped to culprits in advance) applied a second time found in one step the root cause that 4.15's longer investigation had only worked around. Good concrete example of a debugging *method* paying compound interest — and of the lesson that a workaround left in place can hide a root cause until something else depends on the same broken mechanism.
- The student **independently executed the add-a-page checklist** (Spotify 3/3) correctly on first try, including dodging the enum/array ordering trap it warns about — the data-driven design's "adding a page is a data edit" claim validated by someone other than its author. (Placeholder labels/actions on the new page were a conscious shortcut, not an error.)
- **Honest self-assessment to preserve:** the student flagged that the overall code structure still feels rough and that the `user_data`/`void *` mechanism hasn't fully landed yet, and plans to study it further — section 11 exists as the study material for exactly this. Concepts introduced this session (structs as data tables, function pointers, `user_data`, `%`//`/` index math) are *explained and used*, not yet independently *produced* — the distinction matters for an honest portfolio reflection.
- The student raised the maintainability question unprompted ("how would someone less tech-savvy edit this?") — led to a mapped-out ladder from small robustness fixes (count macro, auto-linking) through config-as-data to a full layout editor in the companion app, with the deliberate decision to *not* build any of it yet (the app it would configure doesn't exist). Thinking about future users/maintainers, not just the device working, is portfolio-reflection material.

---

## 10. Coding Style Conventions (established earlier, applies to all project code)

Derived from reviewing the student's own APDS-9960 sensor library (a separate project, used as the reference style) and agreed to carry into the streamdeck firmware for consistency:

- **Comment language: English** (the APDS-9960 reference library itself is in Dutch, but English was explicitly chosen going forward).
- **No Doxygen blocks.** Plain comments only.
- File-header comments: datasheet/reference links plus real-world gotchas (hardware quirks a future reader needs to not get burned by), where relevant.
- Section dividers grouped by **hardware/protocol concern**, not class/code structure, e.g.:
  ```cpp
  // Section Name
  // ============================================================
  ```
- Magic numbers get decoded inline (what the value configures) — **datasheet/register-page references only when genuinely necessary to trace back**, not as a reflex on every constant.
- Bit-masking/shifting operations get a plain-English translation of the hardware effect alongside the line, not a restatement of the C operator.
- No comments that merely restate what the code syntax already says.

*Status 26-7-2026: the current `main.cpp` follows these conventions from the start (English comments, concern-based section dividers, decoded constants, gotcha file header) — the old "retroactively apply style" next-step item is satisfied.*

---

## 11. C Concepts Reference (introduced in the multi-page build — study material)

*Things that were new on 26-7-2026. `user_data` especially — flagged as not yet solid; revisit until it sticks, then rewrite this section in your own words (rewritten explanations stick better than read ones).*

### `user_data` and `void *` — the coat check

`lv_obj_add_event_cb(btn, actionButtonEvent, LV_EVENT_CLICKED, (void *)def)` works like a coat check:

1. **Check-in** (in `buildPage`): the last argument hands LVGL "a thing" to store with this specific button — here, a pointer to this button's own row in the data table.
2. **Storage:** LVGL keeps it as `void *` — pointer to *something*, type deliberately erased. LVGL is generic and can't know `ButtonDef` exists. The clerk stores coats, bags, umbrellas — same shelf, no questions asked.
3. **Pick-up** (in the callback): `lv_event_get_user_data(e)` returns the exact same pointer, still as `void *`. The cast `(const ButtonDef *)` is me saying "unwrap this as what I know I checked in."

**Why it exists:** `actionButtonEvent` is ONE function shared by ~30 buttons. When it fires it must answer "which button was I?" — the answer is whatever was checked in with that widget. Each button carries a pointer to its own identity. Without this: 30 separate callback functions.

**Full trace of one button's life:** boot → `buildPage` reaches `i=1` of `spotifyButtons` → `def` points at the `"Next" / "SPOTIFY:NEXT"` row → that pointer is checked in with the newly created button. Later: tap → LVGL finds that widget's stored callback + pointer → calls `actionButtonEvent` → cast unwraps → `def->action` → `Serial.println("SPOTIFY:NEXT")`. The table row and the on-screen button are permanently linked by one stored pointer.

### Structs and arrays of structs

A `struct` bundles variables under one name (`ButtonDef` = label + action + target traveling together). `.` accesses fields on a value, `->` through a pointer — same rule as `data->state` in the touch callback. An array of structs, each `{ ... }` filling one struct's fields in order, is the data table the whole UI is generated from.

### Function pointers

`makeBarButton(..., lv_event_cb_t callback, ...)` — a parameter that holds *which function to call*. These were in use all along without the name: passing `backButtonEvent` to `lv_obj_add_event_cb` passes the function itself as a value. The helper just makes it explicit — the caller picks the callback the same way it picks the x-position.

### Smaller ones

- `LV_SYMBOL_COPY "\nCopy"` — adjacent string literals are glued into one string at compile time; the symbol macros are themselves just short strings (special font characters), so this yields icon + newline + text in one label.
- `i % gridCols` / `i / gridCols` — recover column and row from a flat index (the inverse of the old nested row/col loops), feeding into the familiar `start + index * stride`.
- `sizeof(pages) / sizeof(pages[0])` — element count of an array (total bytes / bytes per element); never goes stale when rows are added.
- `NULL` as sentinel — one field doing double duty ("no action" *means* "this is a navigation button").

---

## 12. Immediate Next Steps for Next Session

*(as of 26-7-2026 — old list fully superseded; items 1–5 of the old list are done, old item 6 carried forward as item 3 below, old item 7 carried as item 7, old item 8 satisfied per section 10 note)*

1. **Verify serial output explicitly** — open the monitor, press action buttons, confirm clean `SPOTIFY:NEXT`-style lines appear. Navigation and press-highlights are hardware-verified; the serial lines themselves haven't been explicitly checked off, and the entire companion app builds on them. 30-second check, do it first.
2. **Spotify 3/3** — fill in real content (labels/icons currently don't match the actions sent), or park the page by setting page 2's `nextPage` back to `-1` until needed.
3. **Start `companion-app/`** — `spotipy` standalone first: OAuth + play/pause/next working from a plain Python script in a terminal, no ESP32/serial involved yet.
4. **Serial glue** — `pyserial` reading lines from the ESP32, dispatching on the `CATEGORY:ACTION` protocol (section 2.1). Start with `KEY:` actions via keystroke simulation and `SPOTIFY:` via the spotipy work from step 3.
5. **Robustness pass on the data tables** (small, do whenever convenient): `COUNT(arr)` macro so `buttonCount` can't be miscounted; optionally auto-link `prevPage`/`nextPage` (and the "n/m" title counts) at boot from a category field, removing the two most error-prone steps of the add-a-page checklist.
6. **Long-term roadmap item (parked deliberately):** layout editor in the companion app — device stores a layout pushed from the PC over serial, so changing buttons never requires compiling firmware. End-game for maintainability by non-technical users. Explicitly not started until the companion app exists at all.
7. Consider testing the ESP32-S2 mini again at some point out of curiosity (not urgent, no longer blocking anything) — possibly with a multimeter check of VBUS/GND voltage, which was never actually done.