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

**Serial protocol (planned):** simple line-based text over USB serial.
- ESP32 → PC: `BTN:7` (button press events)
- PC → ESP32: `TRACK:...`, `SCENE:...`, `MUTE:1` (status pushes, for showing now-playing track / highlighting active OBS scene / mic-mute indicator on screen)
- OBS pushes events instantly (websocket). Spotify has no push mechanism — PC app must poll `currently-playing` periodically (a few seconds' lag is fine and expected).

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
| 3c | LVGL setup (replaces manual drawing/hit-testing with a real UI library) | 🟡 **In progress** — library installed, config wired, display flush + touch read callbacks working, DRAM overflow fixed, first widget (single button + label) rendering and confirmed on screen |
| 3d | LVGL: generalize to full 6-button grid using LVGL widgets | ⏳ **Next immediate step** |
| 3e | LVGL: styling (background/button colors) | 🟡 Started — background color and button color style calls introduced (`lv_obj_set_style_bg_color`), not yet applied across the full grid |
| 4 | USB HID | ❌ **Dropped as a requirement** — companion app handles keystroke simulation instead (see section 2) |
| 5 | Wire button grid → serial (`BTN:n` messages) | 🟡 Partially done — manual hit-test version (3b) prints `BTN:n` correctly; needs porting to the LVGL version once 3d is done (LVGL button click events will replace polling `whichButton()`), and needs the PC side built |
| 6 | Two-way serial protocol (status pushes back to display: track info, scene state, mute state) | ⬜ Not started |
| 7 | Python companion app — Spotify (`spotipy`) | ⬜ Not started |
| 8 | Python companion app — OBS (`obsws-python`) | ⬜ Not started |
| 9 | Polish (icons, config format, case, etc.) | ⬜ Not started |

**Toolchain/infra status (all done, not part of the numbered roadmap but consumed real time):**
- ✅ Arduino IDE toolchain fully working on classic ESP32 (superseded by PlatformIO but was the original proof-of-concept environment)
- ✅ Switched to PlatformIO + VS Code, fully working build/upload/monitor cycle
- ✅ Git version control set up (command line), repo published to GitHub
- ✅ LVGL 9.5.0 installed and wired to TFT_eSPI (flush + touch read callbacks), DRAM budget fixed

---

## 8. Current Working Code (as of this log)

**Manual hit-test version (superseded by LVGL work but kept here as the last known-good pre-LVGL state):**
```cpp
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

uint16_t calData[5] = { 230, 3539, 255, 3493, 5 };

int size = 120;
int gap = 20;
int startX = 40;
int startY = 40;
int maxRows = 2;
int maxColumns = 3;

void setup() {
  tft.setTouch(calData);
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  for(int currentRow = 0; currentRow < maxRows; currentRow++){
    for(int currentColumn = 0; currentColumn < maxColumns; currentColumn++){
      int x = startX + currentColumn * (size + gap);
      int y = startY + currentRow * (size + gap);
      tft.fillRect(x, y, size, size, TFT_BLUE);
    }
  }
}

void whichButton(uint16_t touchX, uint16_t touchY){
  for(int currentRow = 0; currentRow < maxRows; currentRow++){
    for(int currentColumn = 0; currentColumn < maxColumns; currentColumn++){
      int x = startX + currentColumn * (size + gap);
      int y = startY + currentRow * (size + gap);

      if(touchX >= x && touchX <= x + size && touchY >= y && touchY <= y + size){
        int buttonNumber = currentRow * maxColumns + currentColumn + 1;
        Serial.print("BTN:");
        Serial.println(buttonNumber);
      }
    }
  }
}

void loop() {
  uint16_t touchX, touchY;
  if(tft.getTouch(&touchX, &touchY)) {
    whichButton(touchX, touchY);
  }
}
```

**Current LVGL bring-up version (in progress — single button confirmed working on hardware):**
```cpp
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

TFT_eSPI tft = TFT_eSPI();

static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 320;

// Partial line-buffer, not a full-screen buffer — a full screen's worth of
// pixels doesn't fit in available RAM alongside everything else LVGL/Arduino
// needs. LVGL redraws in horizontal strips instead. Sized at screenWidth * 20
// after a DRAM overflow at screenWidth * 40 (see section 4.12).
static uint16_t buf1[screenWidth * 20];

uint16_t calData[5] = { 230, 3539, 255, 3493, 5 };

// Tells LVGL how to push its rendered pixels to the real screen
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, true);
  tft.endWrite();

  lv_display_flush_ready(disp);
}

// Tells LVGL how to check for touch, using the existing tft.getTouch()
void my_touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
  uint16_t touchX, touchY;
  if (tft.getTouch(&touchX, &touchY)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = touchX;
    data->point.y = touchY;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.setTouch(calData);

  lv_init();

  lv_display_t *disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touch_read);

  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x1E1E1E), 0);

  lv_obj_t *btn = lv_button_create(lv_screen_active());
  lv_obj_set_size(btn, 120, 120);
  lv_obj_set_pos(btn, 40, 40);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF6B00), 0);

  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, "1");
}

void loop() {
  lv_timer_handler();
  delay(5);
}
```

Confirmed on hardware: dark background, one orange 120×120 button at (40,40) with white "1" label, correctly rendered via the flush/touch callback pipeline. Not yet generalized to all 6 buttons (3d, next step).

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

---

## 10. Coding Style Conventions (established this session, applies going forward to all project code)

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

---

## 11. Immediate Next Steps for Next Session

*(as of this log update)*

1. Generalize the current single-button LVGL code into the full 6-button grid: loop over `currentRow`/`currentColumn` (same math as sections 3b/8) and call `lv_button_create` + `lv_label_create` once per button instead of once total. Figure out where per-button click handling will hook in (LVGL event callbacks, e.g. `lv_obj_add_event_cb`) to eventually replace the manual `whichButton()` polling loop.
2. Once the 6-button LVGL grid renders and is stylistically settled (colors applied consistently, not just on button #1), revisit whether `whichButton()`/manual hit-testing code should be removed from `main.cpp` entirely now that LVGL owns touch handling, or kept commented for reference.
3. Wire LVGL button click events to print `BTN:1`–`BTN:6` over serial (replacing the old `Serial.println` calls in the manual `whichButton()`), completing roadmap item 5 for the LVGL version.
4. Start scaffolding the Python companion app (`companion-app/`) — begin with `spotipy` working standalone in a terminal (no ESP32 involved) before wiring it to serial.
5. Consider testing the ESP32-S2 mini again at some point out of curiosity (not urgent, no longer blocking anything) — possibly with a multimeter check of VBUS/GND voltage, which was never actually done.
6. Apply the new comment-style conventions (section 10) retroactively to `main.cpp` if/when convenient — not urgent, but worth doing before the file grows much larger.