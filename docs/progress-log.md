# StreamDeck Project — Progress Log

**Purpose of this document:** full context dump for continuing this project in a future chat, and source material for a later portfolio item. Written in enough detail that someone (or an AI) with zero prior context could pick this up and continue.

**Last updated:** 3-8-2026, 01:06

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

**Serial protocol (original plan, superseded — see v2/v3 below):** simple line-based text over USB serial.
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

### 2.3 Serial protocol v3 (1-8-2026) — the category names the *mechanism*, not the app

Refinement of v2, prompted by the student asking whether categories should be app-based and nestable (e.g. `DISCORD:KEY:CTRL+SHIFT+M`) to support users adding arbitrary things later.

**Decision: the category names how the PC should *execute* the action, not which app it belongs to.**

| Category | Mechanism |
|---|---|
| `KEY:` | simulate keystrokes (incl. media keys) via pynput |
| `SHELL:` | run a command / launch a program — **planned, nothing built on either side yet** |
| `SPOTIFY:` | Windows SMTC media session (decided 3-8-2026, see 2.5 — was: Spotify Web API via spotipy). Not yet wired into the dispatcher. |
| `OBS:` | OBS websocket via obsws-python (not implemented yet) |

**Reasoning:** the Python side's only real decision is "how do I execute this?" A Discord mute via keystroke and a Copy via keystroke are the *same operation* — same code path. An app-first prefix would mean either duplicating the keystroke logic per app, or stripping the prefix and delegating, which makes the prefix decorative. Grouping-by-app is a *display* concern that belongs in future config metadata, not in the wire protocol.

**Consequences applied to the firmware:**
- `MEDIA:PLAYPAUSE` → `KEY:MEDIA_PLAYPAUSE` (and the other five media buttons) — **done, working**
- `SYS:LOCK`, `SYS:SLEEP` → `SHELL:...` (both need real commands, see 4.18) — **done 2-8-2026**
- `DISCORD:*` → `KEY:CTRL+SHIFT+M` etc., since Discord's mute/deafen are just global hotkeys — **done 2-8-2026**
- `SPOTIFY:` shrinks to only what media keys can't do (Like, seek, queue, reading now-playing); transport controls can go through `KEY:MEDIA_*` and work with no OAuth at all
- The `SYS:` and `MEDIA:` categories disappear entirely

**On "infinite" extensibility:** the category set is bounded by what the Python app knows how to do (~4 categories, ever), *not* by what users want on buttons. User freedom comes from (a) `KEY:` and `SHELL:` being generic escape hatches that can trigger essentially anything on the machine, and (b) free-form layout — any action on any button on any page.

> **Migration complete 2-8-2026.** All three consequences above are now applied. No pre-v3 strings (`SYS:`, `MEDIA:`, `DISCORD:`) remain anywhere in the firmware — every button on every page sends either `KEY:` or `SHELL:`, or is a navigation button sending nothing. The wire protocol is fully v3.

### 2.4 Discord: keybinds, not an API (decided 2-8-2026)

Considered properly rather than defaulted into, since every other integration in this project reaches for an API.

**There is no supported Discord API for this.** The public API is for *bots* — server management, messages, slash commands. Muting your own client's microphone is local client state, not a bot operation. The bot API can server-mute *someone else*, which is a moderation action and a completely different thing.

What does exist is Discord's **RPC API** — a local socket with real mute/deafen control and voice-state events, which is what the official Elgato Streamdeck Discord plugin uses. But access needs an approved application with the RPC scope whitelisted (discretionary, aimed at established integrations), and the API is only semi-documented and has changed without notice. So the comparison is not "hacky keybinds vs clean API" — it's "supported user-facing feature vs undocumented endpoint requiring permission."

**The only thing an API would buy is state.** A keybind is fire-and-forget: the device sends `CTRL+SHIFT+M` and has no idea whether the result is muted or unmuted. A button can't display current state, and if the two desync (muted with the mouse instead), the display would lie. RPC gives events, so the display could show truth.

**Decision: keybinds now, revisit RPC only after roadmap item 6 exists.** Two-way serial is needed anyway for now-playing text and OBS scene state; Discord state would then be a fourth consumer of infrastructure built for other reasons, not a new mechanism.

### 2.5 Spotify: Windows SMTC instead of the Web API (decided 3-8-2026)

**The biggest architectural change since protocol v3, and it removes a dependency rather than adding one.**

The original plan (26-7-2026) assumed Spotify meant OAuth and `spotipy`. Checking the current state of the Web API first turned up hard constraints that have tightened since that plan was written:

- **Premium is now required to use the Web API at all** — not just for playback endpoints. Development-mode apps stop working if the owner's Premium lapses.
- **Development mode is capped at five authenticated users** via a manual allowlist in the developer dashboard, down from 25. Non-allowlisted users get 403s *after* successfully authenticating — auth works, authorisation doesn't, which is a confusing failure mode.
- Extended quota is closed to individuals (requires a registered business, launched service, 250k+ MAU), so development mode is permanent for a personal project.
- Each additional user must be added by hand **and** hold their own Premium subscription.
- Plus the 6-month refresh-token expiry already noted from the earliest planning session, and the February 2026 endpoint removals.

**The alternative found: Windows SMTC** (`GlobalSystemMediaTransportControlsSessionManager`). Windows exposes every media app's session — it's what powers the volume-overlay media popup. Crucially this is **not** media keys: sessions can be enumerated and filtered by `source_app_user_model_id`, so Spotify can be targeted *specifically* rather than broadcasting to whatever the OS thinks is active. That solves the wrong-app problem that made media keys unreliable.

| | SMTC | Spotify Web API |
|---|---|---|
| Play/pause, next, previous | ✅ | ✅ |
| Seek | ✅ (absolute position) | ✅ |
| Shuffle / repeat | ✅ **and readable as state** | ✅ |
| Now-playing title + artist | ✅ (one artist) | ✅ (full artists array) |
| **Like / save track** | ❌ | ✅ |
| Setup required | `pip install` | dashboard app, OAuth, Premium, allowlist |
| Ongoing constraints | none | Premium, 5 users, 6-month token |
| Portability | Windows only | any OS |

**Decision: SMTC for everything; the Web API is now optional and needed only for Like.** Verified on hardware 3-8-2026 — both reading (`try_get_media_properties_async`) and control (`try_skip_next_async`) work against the desktop Spotify client with zero account setup.

**Rejected alternative — intercepting the desktop client.** The idea of sniffing what Spotify sends when "next" is pressed and replaying it: the client talks to Spotify's internal API over TLS with its own credentials, so it would need MITM with a custom root cert, targets undocumented endpoints that change without notice, and sits against the ToS. The old local `SpotifyWebHelper` HTTP server on port 4381 that made this viable years ago was removed by Spotify. Not pursued.

**Consequence for the page layout:** three Spotify pages collapse to one. Everything except Like is available, and "Queue" was never coherent as a button anyway (queue *what*? — the parameter problem from 8.4). Volume stays on the Media page as `KEY:MEDIA_*`; SMTC is a media-session API and has no volume concept.

**Cost accepted:** a second and deeper Windows dependency. `SHELL:` strings could be swapped per-OS in a config file; SMTC would need a completely different implementation on Linux (MPRIS over D-Bus — same idea, different plumbing). Given `rundll32` is already in the companion app, this doesn't lose portability that existed.

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
- **Recurs on the Python side (1-8-2026):** the companion app and PlatformIO's monitor compete for the same port. Only one program can hold COM13 — close the monitor before running the Python app, and vice versa.

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

### 4.18 Win+L cannot be simulated — Windows blocks it at OS level (1-8-2026)
- `KEY:WIN+L` (lock PC) fired from the device produced only a typed letter `l` in the focused window. Every other combo tested worked fine.
- **Cause:** Win+L sits in the same protected category as Ctrl+Alt+Del — Windows intercepts it below the normal input layer, deliberately, so that no program can lock the machine or fake a lock screen. pynput emits the events; Windows swallows the Win part and the `l` falls through to whatever has focus.
- **Not a code bug** — it will never work through keystroke simulation, from any library.
- **Fix:** use the Windows API call instead, via the `SHELL:` category (see 2.3):
  ```
  SHELL:rundll32.exe user32.dll,LockWorkStation
  ```
  Same reasoning applies to sleep: `rundll32.exe powrprof.dll,SetSuspendState 0,1,0`. Neither is implemented yet — the `SHELL:` handler is still to be written.
- **Lesson worth keeping:** "the OS refuses to do this on purpose" is a real category of cause, distinct from a bug. Worth suspecting whenever exactly one shortcut fails while everything else works.

> **Addendum 2-8-2026 — CLOSED.** The `SHELL:` handler now exists on both sides and `SHELL:rundll32.exe user32.dll,LockWorkStation` locks the PC correctly from the device. Sleep also works, with the caveat in 4.22.

### 4.19 `SetSuspendState 0,1,0` hibernated instead of sleeping (2-8-2026)
- The first argument `0` is documented as "sleep, do not hibernate." Windows **ignores it**: if hibernation is enabled on the machine, this call hibernates regardless. Observed directly — the button hibernated the PC.
- Not a bug in the command string or in the `SHELL:` handler, and no code change fixes it.
- **Fix applied:** `powercfg /hibernate off` in an elevated prompt. Hibernation now disabled system-wide, so the same command produces real sleep.
- Sleep vs hibernate, since the distinction mattered here: **sleep** keeps RAM powered (very low draw, ~2s resume, session lost if power is cut); **hibernate** writes RAM to disk and powers off completely (zero draw, slower resume, several GB written to the SSD each time).

### 4.20 Packaged (MSIX/Store) apps cannot be launched by file path (2-8-2026)
- Wanting a "launch Claude" button, the usual route failed at the first step: right-clicking the Start menu entry opens the app's *settings page*, not a Properties dialog with a Target field. There is no path to copy.
- Two separate obstacles, and the second is the one that matters:
  - **Permissions:** `C:\Program Files\WindowsApps` is owned by `TrustedInstaller` and denies read access even to Administrators. Ownership *can* be taken, but Windows Update can reset it.
  - **App containers (the real blocker):** packaged apps run inside a container whose identity is assigned *at activation* — Windows reads the manifest, sets up the sandbox, and only then starts the process. Launching the exe directly skips all of that, so the app is missing context it expects. Not a permissions problem you can grant your way out of.
- **Solution — launch by identity, not by path:**
  ```
  SHELL:explorer shell:AppsFolder\Claude_pzs8sxrjxfjjc!Claude
  ```
  `explorer` (the Windows shell process) is what resolves app identities; `shell:AppsFolder` is a shell *namespace* address pointing at a virtual list of installed app identities, not a folder on disk.
- **How to obtain the ID:** Win+R → `shell:AppsFolder` → right-click the app → Create shortcut → accept "put it on the desktop" → right-click that shortcut → Properties → copy the **Target** field. The desktop shortcut can be deleted immediately afterwards; it was only ever a way to make Windows display the ID.
- **⚠️ CORRECTION (3-8-2026).** The original version of this entry claimed the hash was "per-install and not derivable", invalid after a reinstall and on other PCs. **That was wrong**, and it was an AI-stated claim accepted without checking at the time. The Publisher ID is a Crockford Base32 encoding of the first 8 bytes of the SHA-256 hash of the publisher string — a deterministic algorithm that is constant on all machines. The string is the same for any package signed by the same certificate, and can be computed without installing the package at all. So `Claude_pzs8sxrjxfjjc!Claude` is **the same on every machine** that installs packaged Claude, and survives reinstalls. It only changes if Anthropic changes its publisher certificate. (Caught by asking "will the IDs always be the same for my device and for other people's devices?" — see section 9.)
- Note the entry point after `!` varies per package (`!Claude` here, `!App` in many others) — a package names its own entry points.
- **Generalisable rule:** for packaged apps, **the identity is the address, not the path.** Same instinct as the protocol itself — name the thing you want and let the other side resolve it. Names survive the implementation moving.

### 4.21 Four unrelated meanings of "shell" in one command line (2-8-2026)
Genuinely confusing while building the Claude button, and worth decoding once:
```
SHELL:explorer shell:AppsFolder\Claude_pzs8sxrjxfjjc!Claude
└─1─┘ └───2──┘ └──────────────────3──────────────────────┘
```
1. **`SHELL:`** — this project's own protocol category (v3, section 2.3). Python strips it at `split(":", 1)`; nothing downstream ever sees it.
2. **`explorer`** — the executable to run. Explorer isn't just the file browser; it *is* the Windows shell process, and it's what knows how to activate packaged apps.
3. **`shell:AppsFolder\...`** — a Microsoft shell-namespace address. The `shell:` prefix means "this is a namespace address, not a file path" (cf. `shell:Downloads`, `shell:RecycleBinFolder`).
4. Not visible in the line but present in the code: **`shell=True`** in Python's `subprocess.Popen`, meaning "let `cmd` parse this string."

Four different things named "shell", none related to each other. Worth writing down because "why do we do SHELL:explorer shell:AppsFolder" was a real point of confusion, and the answer is that it's three different vocabularies colliding.

### 4.22 Compile error from a stray comma in a `ButtonDef` row (2-8-2026)
- Written as `{ LV_SYMBOL_EDIT, "\nNotepad", "SHELL:notepad", 0 },` — one extra comma.
- **Cause:** every label in this codebase relies on **adjacent string literals being glued together at compile time** (`LV_SYMBOL_EDIT "\nNotepad"` is *one* string). A comma between them makes them two separate initialisers, so a 3-field struct receives 4 values: `label` gets the symbol, `action` gets `"\nNotepad"`, `targetPage` gets a string where an `int` belongs.
- Trivial to fix (delete one character) but worth recording because it is **specific to the label convention this project uses everywhere** — the same typo in a normal struct would look obviously wrong; here it looks almost right.

### 4.23 `winrt` package lineage — most tutorials import a dead package (3-8-2026)
- Python's WinRT bindings have changed hands three times: Microsoft's monolithic `winrt` (2019–2021) → community `winsdk` (2022–2023) → since September 2023, **modular per-namespace packages**, with the top-level namespace reverted from `winsdk` back to `winrt`.
- `winsdk` is explicitly deprecated in favour of per-namespace `winrt-Namespace` packages. Any tutorial importing `winsdk.windows.media.control` is on the old lineage — same API, different import path.
- **Current:** `pip install winrt-Windows.Media.Control`, providing the `winrt.windows.media.control` module. Version 3.2.1 at time of writing.

### 4.24 Cascading `ModuleNotFoundError` from the per-namespace split (3-8-2026)
Two consecutive failures, both naming modules that appear nowhere in the source:
- `No module named 'winrt.windows.foundation'` — thrown by `await MediaManager.request_async()`, because it *returns* an `IAsyncOperation`, which lives in the Foundation namespace. Fix: `pip install winrt-Windows.Foundation`.
- `No module named 'winrt.windows.foundation.collections'` — thrown by `manager.get_sessions()`, because it *returns* a WinRT vector. Fix: `pip install winrt-Windows.Foundation.Collections` (a **separate** package despite the name looking like a subpackage — WinRT treats `Windows.Foundation.Collections` as its own namespace and the packaging mirrors namespaces exactly).
- **General rule:** each `winrt-*` package contains only its own namespace. If a method *returns* a type from another namespace, that package is needed too, even though the code never names it. Read the missing module out of the traceback and install the matching package.
- Four packages total were needed for one import line: `winrt-Windows.Media.Control`, `winrt-runtime`, `winrt-Windows.Foundation`, `winrt-Windows.Foundation.Collections`.
- Useful diagnostic detail: the two errors came from **different line numbers** (line 7 then line 8), which confirmed real progress between them rather than the same failure repeating.

### 4.25 Returned the ID string instead of the session object (3-8-2026)
```python
session = lower(session.source_app_user_model_id)   # two bugs in one line
```
- **`lower` is a string method, not a builtin** — `x.lower()`, not `lower(x)`.
- **The loop variable was reassigned to a value derived from itself**, so `session` stopped being the session object and became the string `"spotify.exe"`. `return session` then handed back a string, and the caller's `session.try_get_media_properties_async()` failed because strings have no such method.
- **This is the same habit as the `KEY:` resolve loop** (`for key in keys: key = function_resolve_keys(key)`), where it was harmless. Here it silently destroyed the object that was actually needed. Fix: give the derived value its own name (`app_id`), keep the object intact.
- **Mental model that fixes it permanently:** the session is an *object* carrying methods; `source_app_user_model_id` is just a *label* on it. Inspect labels to choose an object — then keep the object, not the label.

### 4.26 `for`/`else` used where plain fall-through was meant (3-8-2026)
- After moving the `return None` out of the loop body, it was indented as `else:` attached to the `for`. This is **valid Python**, not an error — but `for`/`else` runs the `else` only when the loop completes **without hitting a `break`**. With no `break` present it always runs, so it happened to behave correctly *by accident*.
- Replaced with an unindented `return None` after the loop. Same behaviour, expressed by control flow rather than by a keyword that means something adjacent — and it won't silently change meaning if a `break` is added later.
- Worth recording as a near-miss: the instinct about *where* the fallback belonged was right; the construct reached for was wrong.

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

**Serial monitor in PlatformIO:** plug icon 🔌 in the bottom VS Code toolbar, or `pio device monitor` in terminal. Respects `monitor_speed` from the ini file automatically. **Note:** leaving a monitor session open blocks the next upload attempt on that COM port (see 4.13) — close it before re-uploading, and also before running the Python companion app.

**Python side (added 1-8-2026):** plain Python, no virtualenv yet. Dependencies: `pip install pyserial pynput`. Note the import mismatch — the package is `pyserial` but the import is `import serial`.

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
└── companion-app/            (Python companion app — started 1-8-2026)
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
| 5 | Wire button grid → serial | ✅ **Done (1-8-2026)** — action strings confirmed arriving in the Python app; protocol upgraded to v3 (see 2.3) |
| 6 | Two-way serial protocol (status pushes back to display: track info, scene state, mute state) | ⬜ Not started |
| 7a | Python companion app — keystroke dispatcher | ✅ **Done (1-8-2026)** — reads serial, parses `CATEGORY:ACTION`, resolves arbitrary key combos, executes them. Keybinds page and Media page both confirmed working end-to-end on hardware. **Written by the student**, incrementally. |
| 7b | `SHELL:` category — Python handler **and** firmware action strings | ✅ **Done (2-8-2026)** — `subprocess.Popen(action, shell=True)` on the Python side, five System-page buttons on the firmware side (Lock, Sleep, Notepad, Claude, VS Code). Lock and Sleep both confirmed on hardware; see 4.19–4.21 for the Windows-specific gotchas. |
| 7c | Python companion app — Spotify | 🟡 **Approach changed 3-8-2026: SMTC instead of `spotipy`** (see 2.5). Read *and* control both proven against desktop Spotify with zero account setup — transport, seek, shuffle, repeat, now-playing all available. `spotipy` now optional and needed only for a Like button. Category not yet wired into the dispatcher. |
| 7d | Discord — migrate to `KEY:` global keybinds | ✅ **Done (2-8-2026)** — 6 buttons, all verified on hardware with Discord unfocused. No API needed; see 2.4 for why, and 8.4 for what keybinds can't reach. |
| 8 | Python companion app — OBS (`obsws-python`) | ⬜ Not started |
| 9 | Polish (icons, config format, case, etc.) | ⬜ Not started — roadmap idea: long-term, a layout editor in the companion app (device stores a layout pushed from the PC) so button changes never require compiling firmware; see section 12 |

**Toolchain/infra status (all done, not part of the numbered roadmap but consumed real time):**
- ✅ Arduino IDE toolchain fully working on classic ESP32 (superseded by PlatformIO but was the original proof-of-concept environment)
- ✅ Switched to PlatformIO + VS Code, fully working build/upload/monitor cycle
- ✅ Git version control set up (command line), repo published to GitHub
- ✅ LVGL 9.5.0 installed and wired to TFT_eSPI (flush + touch read callbacks), DRAM budget fixed
- ✅ `lv_tick_inc()` wired in `loop()` — mandatory for LVGL timers on Arduino (see 4.16)
- ✅ Python environment working (`pyserial` + `pynput`); full touchscreen → ESP32 → USB → Python → real keystroke pipeline proven on hardware

---

## 8. Current Working Code (as of this log)

**Full current firmware and companion app: see git, as of 3-8-2026** (`firmware/src/main.cpp`, `companion-app/`). Older versions live in git history — no longer embedded here. Note the SMTC work of 3-8-2026 currently lives in a standalone `test.py` and is **not yet merged into `main.py`** — see section 12 item 1.

### 8.1 Firmware architecture (multi-page, data-driven)

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
- **Protocol v3 migration COMPLETE (2-8-2026):** Media sends `KEY:MEDIA_*`; System sends `SHELL:` (5 buttons); Discord sends `KEY:` global keybinds (6 buttons). No `SYS:`, `MEDIA:` or `DISCORD:` strings remain. Page tree unchanged; only button counts moved (System 2 → 5, Discord 4 → 6).
- **Final Discord page (2-8-2026), all six verified on hardware with Discord unfocused:**

  | Button | Action string | Source of the combo |
  |---|---|---|
  | Mute | `KEY:CTRL+SHIFT+M` | Discord default |
  | Deafen | `KEY:CTRL+SHIFT+D` | Discord default |
  | Hang up | `KEY:CTRL+SHIFT+P` | **self-assigned** |
  | Screenshare | `KEY:CTRL+SHIFT+I` | **self-assigned** |
  | Overlay | ``KEY:SHIFT+` `` | Discord default (action is named **Toggle Overlay Lock** in the dropdown) |
  | Streamer | `KEY:CTRL+SHIFT+S` | **self-assigned** |

  Notes for a future session: the backtick needs no `KEY_NAMES` entry — it's one character, so the resolver's `len(key) == 1` branch passes it to pynput as a plain string. The overlay action is **not** called "Overlay" in Discord's dropdown; there are three separate overlay actions (Toggle Overlay, Toggle Overlay Lock, Activate Overlay Chat) and only the middle one matches Shift+`. Overlay actions are hidden from the dropdown entirely unless the overlay is enabled under Settings → Game Overlay, and the overlay is Windows-only. Also: **Discord disables all keybinds while the Keybinds settings page is visible**, so testing from the device with that page open shows nothing and looks broken. Assign keybinds under Settings → Keybinds rather than under Game Overlay — there is a long-standing bug where recording it on the latter page clears it on navigation. `CTRL+SHIFT+S` collides with Save As in many apps; Discord wins while running, but worth changing if it ever conflicts.
- ⚠️ **The `pages[]` `buttonCount` was forgotten twice in one session** when adding buttons to System and Discord — the single most repeated mistake in this project so far. This is exactly what the planned `COUNT(arr)` macro prevents, and it has been promoted to next-steps item 1 as a result.

### 8.2 How to add a page (validated 26-7-2026 by adding Spotify 3/3 independently)

Four touches, all in the data section, zero logic changes:

1. **Button array** — define `xyzButtons[]`, up to 6 entries.
2. **Enum** — add `PAGE_XYZ` *in the same position* the page will have in `pages[]`.
3. **`pages[]` row** — insert the entry at that same position.
4. **Re-link** — if it extends a category chain: previous page's `nextPage` → new page, new page's `prevPage` → back. Update the "n/m" counts in the title strings of every page in the chain (the counts are plain data in the titles, nothing computes them).

**THE trap:** enum and `pages[]` are connected only by order. Adding the enum entry in one place and the array row in another silently shifts every later page index onto the wrong screen. Insert both in the same spot, always.

Convention decided: back from any page in a chain goes straight to home (`parentPage = PAGE_HOME` for all of them); prev/next handle movement within the chain, so back never duplicates prev.

Known placeholder: Spotify 3/3's labels/icons are copy-paste artifacts that don't match the actions its buttons actually send. Harmless as a navigation test, must be filled in or parked before relying on it.

### 8.3 Companion app architecture (Python, written 1-8-2026)

Single script, no classes. Flow:

1. **Open the port** — `serial.Serial(comPort, baudRate, timeout=1)`. Port and baud hardcoded (`COM13`, 115200); baud must match `Serial.begin()` in the firmware or the bytes are gibberish.
2. **Read loop** — `connection.readline()` returns raw **bytes** ending in `\r\n`. Decoded with `errors="replace"` (the ESP32 emits boot garbage when the port opens, because opening it toggles DTR/RTS and resets the board) and `.strip()`ed. An empty result means the 1s timeout expired with nothing received; skipped via `if not line: continue`. That timeout is also what keeps Ctrl+C responsive.
3. **Split** — `line.split(":", 1)` into category + action. The `1` limit matters so `OBS:SCENE:1` keeps `SCENE:1` intact. Guarded by `if ":" not in line: continue` — without it, boot noise containing no colon raises `ValueError` on the two-name unpack.
4. **Resolve** (`KEY:` only) — `action.split("+")` gives individual key names; each goes through `function_resolve_keys()`: in the `KEY_NAMES` dict → return the pynput object; single character → return it lowercased (pynput accepts plain strings); otherwise → `None`. If any name resolved to `None`, the line is reported and skipped **before anything is pressed** — otherwise a modifier could be left stuck down with nothing to release it.
5. **Execute** (`function_execute_keybinds()`) — `resolvedKeys[:-1]` are held down, `resolvedKeys[-1]` is tapped, then the held keys are released in reverse order. This ordering *is* the keybind: the tapped key must go down while the others are already held, or the OS just sees separate keystrokes.

`KEY_NAMES` maps firmware-side names to pynput objects, including media keys (`MEDIA_PLAYPAUSE` → `Key.media_play_pause`, etc.). The `MEDIA_` prefix was added deliberately to avoid a future collision with a plain `NEXT`/`PREV`. Note `WIN` maps to `Key.cmd` — pynput is cross-platform and names the Windows key after the Mac Command key.

**Payoff of the parsing approach:** adding a keybind that only uses already-known keys requires **no Python changes at all** — it's a firmware-only edit. A keybind using a new named key is one dict entry.

**Known assumption (discussed, deliberately kept):** everything except the last key is treated as held. Nothing validates that the held keys are actually modifiers, so `KEY:C+CTRL` would hold "c" and tap Ctrl without complaint. Left permissive on purpose — it's what makes non-standard combos like Discord's `CTRL+N+P` work (Discord does its own key-state tracking rather than using Windows' registered-hotkey API).

**Known limitation:** *sequential* combos (VS Code's `Ctrl+K Ctrl+C`) can't be expressed — the format has no way to say "release everything, then do another combo." Solvable either firmware-side (two `Serial.println` calls, zero Python changes) or Python-side (a `;` separator). Not built; noted as solvable when needed.

**`SHELL:` handler (added 2-8-2026)** — three lines: `import subprocess`, an `elif category == "SHELL":` branch, and `subprocess.Popen(action, shell=True)`.

- **`Popen`, not `run`** — `run()` blocks until the launched program exits, which would freeze the `while True` read loop for as long as (say) Notepad stays open, while button presses pile up unread in the serial buffer. `Popen` starts the process and returns immediately. For a dispatcher, non-blocking is the whole requirement.
- **`shell=True`** hands the string to `cmd` to parse, rather than treating it as one literal executable name — that's what makes `rundll32.exe user32.dll,LockWorkStation` work as a command *line* with arguments. Standard caveat: it will run anything given to it. Acceptable here because the input arrives from this project's own firmware over a USB cable; would not be acceptable for strings arriving from a network.
- **⚠️ `shell=True` fails silently.** A typo'd command is accepted by `cmd`, fails to find the program, returns non-zero — and `Popen` never looks at the exit code. So "nothing happened" does **not** distinguish "the branch was never reached" from "the command string is wrong." Debug by printing immediately before the `Popen`.
- **`split("+")` moved inside the `KEY:` branch** — it was running for every line regardless of category, doing work only `KEY:` ever consumed.
- **New `else` branch** prints unknown categories instead of dropping them silently. Previously a `SPOTIFY:` press vanished with no trace.

**Dispatch design decision (2-8-2026): keep `if`/`elif`, do NOT build a category dict.** Reasoning, since the instinct to mirror `KEY_NAMES` is natural: `KEY_NAMES` maps **data to data**, has many uniform entries, and grows every time a keybind is added — exactly what a dict is for. Categories map a name to **behaviour**, the set is bounded at ~4 by protocol v3's own logic, and the branch bodies are not uniform (`SHELL:` is one line; `KEY:` is split → resolve → validate → execute; `SPOTIFY:` will need a client built at startup plus its own second-level dispatch). Forcing those into a dict means every value becomes a function with a matching signature — a real structure, not just a tidier `if`. Plan: extract each branch body into its own function (as already done for `function_resolve_keys` / `function_execute_keybinds`), which makes the eventual `HANDLERS = {"KEY": handle_key, ...}` dispatch table a ten-minute change rather than a restructure. Revisit around the fourth category, when `SPOTIFY:` has shown what those handlers actually need to look like.

### 8.4 Limits of the `KEY:` mechanism (mapped 2-8-2026)

Building the Discord page surfaced a clean boundary, and it generalises beyond Discord.

**`KEY:` sends one parameterless event. It can only drive actions that require no decision after the trigger.**

| Works | Doesn't work | Why |
|---|---|---|
| Mute, Deafen, Hang up, Overlay lock, Streamer mode | — | parameterless toggles |
| Screenshare **with a recognised game running** | Screenshare with no game | the target is supplied implicitly by Discord's game detection; without it, a picker appears |
| Camera **off** | Camera **on** | turning on always prompts for a background choice; turning off needs no decision |
| — | Push-to-talk | needs **hold state**, not a tap |

- **Camera was dropped from the page** for exactly this reason — a button that only gets you to a dialog is worse than no button, because it implies an action it doesn't complete.
- **PTT is a different failure:** the protocol sends one event per press, and `function_execute_keybinds` always presses and releases in the same breath. Supporting hold would need `KEYDOWN:` / `KEYUP:` fired from `LV_EVENT_PRESSED` / `LV_EVENT_RELEASED`, with Python holding the key between them. Deliberately not built — a dropped release line would leave a key stuck down forever, which is precisely what the existing validation pass was written to prevent. (A touchscreen is also a poor PTT surface.)
- **Same family as the sequential-combo gap** in 8.3: the protocol carries one event with no payload, and some actions need more.
- **This is exactly the class of problem an API fixes.** `OBS:SCENE:1` carries a parameter where a keybind cannot; Discord RPC would similarly let a screenshare target be named. Another entry in the same ledger as 2.4: *keybinds are fine until you need to name a thing.*

### 8.5 `SHELL:` and `KEY:` action strings are machine-specific — and that's the point

Surfaced by asking whether `SHELL:code` works for everyone who installs VS Code. It mostly does (the installer's "add to PATH" checkbox is ticked by default) — but it's a checkbox, and portable/ZIP installs don't get it. Which prompted looking at the whole set:

| Action string | What it depends on |
|---|---|
| `SHELL:code` | a PATH entry the VS Code installer *optionally* adds |
| `SHELL:explorer shell:AppsFolder\Claude_pzs8sxrjxfjjc!Claude` | packaged Claude being installed — but the ID itself is **portable**, see correction in 4.20 |
| `SHELL:start "" "C:\Users\joost\...\Code.exe"` | a specific username and install location |
| `SHELL:rundll32.exe user32.dll,LockWorkStation` | Windows only |
| `KEY:CTRL+SHIFT+P` (Hang up), `KEY:CTRL+SHIFT+I` (Screenshare), `KEY:CTRL+SHIFT+S` (Streamer) | **self-assigned Discord keybinds** — these are not Discord defaults; they exist only because they were registered by hand in User Settings → Keybinds on this machine |

**This is not a flaw in the protocol — it is what an escape hatch *is*.** `KEY:` and `SHELL:` get their power from passing arbitrary machine-specific instructions through untouched, and the cost of that power is that they encode assumptions about one machine. `SPOTIFY:NEXT` is universal; `SHELL:code` never will be, and doesn't need to be.

**What it does mean: these strings should not live in compiled firmware long-term.** Right now, changing a keybind or reinstalling Claude requires editing C and reflashing — and handing this device to anyone else would mean recompiling for their machine. This is the strongest concrete argument yet for **roadmap item 9 (config pushed from the PC)**: once actions come from a config file, each machine holds its own strings and the firmware becomes genuinely portable. Protocol v3 already supports this by design — an action string is *data*, and can come from a file where a function pointer could not (see section 11). The gap between "works on my PC" and "works on any PC" is now a concrete, motivated feature rather than an abstract nice-to-have.

**Immediate mitigation until then:** a comment in `systemButtons[]` and `discordButtons[]` flagging that these strings depend on this machine's setup, and noting *how* the Claude app ID was obtained (see 4.20) — not because it expires, but because the retrieval route is non-obvious and would be needed again for any other packaged app.

> **Correction note (3-8-2026):** this section originally listed the Claude app ID as machine-specific. It isn't (see 4.20). The section's argument survives — the username-path, PATH-dependency, Windows-only and self-assigned-keybind rows all still stand — but one of its four examples was wrong and has been fixed rather than quietly dropped.

### 8.6 SMTC / async — mechanics and the integration question (3-8-2026)

**The object chain**, since everything hangs off one session object:
```
MediaManager.request_async()          ← await
  └── manager.get_sessions()          ← no await (instant)
        └── [session, ...]            one per media app
              ├── source_app_user_model_id     attribute → "Spotify.exe"
              ├── try_get_media_properties_async()   ← await → title, artist, album_title
              ├── get_playback_info()          → is_shuffle_active, auto_repeat_mode, controls
              ├── get_timeline_properties()    → position, start_time, end_time
              └── try_skip_next_async() / try_play_async() / try_change_playback_position_async()   ← await
```
- **Naming conventions that matter:** `_async` suffix = needs `await`; no suffix = returns immediately. `try_` prefix is a WinRT convention meaning "may legitimately fail" — returns a result rather than raising. Microsoft's docs are C#/PascalCase (`GetCurrentSession`); the Python bindings are auto-generated snake_case (`get_current_session`). **There are no Python API docs** — the bindings are generated, so Microsoft's C# reference is the only source of truth and every name must be translated mentally.
- **Session selection:** matching `"spotify" in app_id.lower()` rather than `== "Spotify.exe"`, deliberately. Desktop Spotify reports `Spotify.exe`; the Microsoft Store build reports a packaged AUMID (`SpotifyAB.SpotifyMusic_<hash>!Spotify`). Both are stable across machines — the fork is install type, not user. The substring match covers both for one line. (Confirmed on this machine: desktop install, `Spotify.exe`, and it was the only session present at the time.)
- **`get_current_session()` exists and is simpler** (one call, no loop) but returns whichever session Windows considers active — the same "whatever the OS thinks" behaviour that makes media keys unreliable. The loop is chosen deliberately, not out of ignorance of the simpler option.
- **Metadata limit accepted:** `artist` holds one artist only; there is no structured list of additional credited artists. Featured artists appear inside the `title` string as text (Spotify's own tagging convention), which is *not* the same as having them as data — extracting them would mean parsing a display string, which is fragile and not worth doing. The Web API's `artists` array is the only real source. **Decided this is fine** — the display use case is one line of text on a 480×320 screen, which would truncate anyway. `subtitle` was empty and `album_title` duplicated the artist on the test track; `title` and `artist` are the only two useful fields.

**The async structural question (open, decide before wiring the category in):** `main.py`'s read loop is ordinary blocking code (`while True` on `connection.readline()`) and cannot `await` anything. Two ways across:
- **`asyncio.run()` inside the `SPOTIFY:` handler** — one fresh event loop per button press. Mildly wasteful, entirely fine at human press rates, no restructuring. Likely choice for now.
- **Make the whole read loop async** — cleaner in principle, restructures the main loop.

This becomes a real decision rather than a formality at **roadmap item 6**: showing now-playing on the display means polling SMTC *while* the serial loop reads presses — two things genuinely in flight at once, which is the first point where async earns its keep rather than being a shape imposed by WinRT.

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

**Added 1-8-2026 (firmware walkthrough + companion app session):**

- **Structured walkthrough of code the student did not write.** The session opened by dissecting the previous session's AI-written firmware in six planned parts (skeleton → hardware glue → data model → page builder → events → end-to-end trace), student-paced, with follow-up questions driving the depth (`lv_timer_handler`, what "dirty" means, `lv_screen_load` and preloading, what `PageDef`'s buttons pointer does, `*` vs `&`). Reading unfamiliar code is a distinct skill from writing it — worth naming as such in the portfolio rather than folding into "learned LVGL."
- **Mid-walkthrough pushback: "this is much complexer with LVGL than it would be normal."** This prompted separating two things being blamed on one cause: complexity LVGL genuinely imposes (screens, dirty areas, callbacks) versus complexity the *chosen architecture* imposes (data tables, generic builder, `user_data` — none of which LVGL requires). An explicit offer was made to rewrite it as a dumber ~700-line, one-explicit-function-per-screen version the student would fully own, with the trade-off stated (the config-from-PC roadmap item would become a rewrite rather than an extension). The student declined and continued. The architecture was therefore **re-consented to, not merely inherited** — worth recording.
- **Pointers were explicitly parked, not faked.** After several explanations (`*` vs `&`, dereferencing, why a copy wouldn't work for `def`), the student said "pointers are confusing" and chose to let it rest for now. Recorded honestly rather than papered over. The working model they *do* hold — "a pointer is a handle to a thing that lives somewhere else; read it with `->`" — is sufficient to read the firmware fluently, and the theory can wait until something forces it.
- **The entire Python companion app was written by the student**, in the same incremental style as the early display work: AI described the pieces and the pitfalls, student wrote every line. Progression: bare serial read → decode/strip → empty-line guard → colon guard → category/action split → `KEY_NAMES` dict → resolver function → validation pass → press/release execution → refactor into two functions. Real bugs made and fixed along the way:
  - `serial.Serial(...)` called without assigning the result (connection object discarded)
  - `serial.readline()` — calling a method on the *module* instead of the connection object
  - `line = print(...)` — assigning `print`'s return value (`None`), then calling `.split()` on it
  - the press/release block indented *outside* the `if category == "KEY"` branch, so a Spotify press would have re-fired a stale keybind from a previous line
  - `=<` instead of `<=`; `return` used at top level outside any function; a missing comma in `print()`
- **The student challenged the design twice, correctly both times.** First: *"I don't think it is smart to assume everything except the last one being modifiers?"* — a legitimate challenge to an unvalidated assumption, which surfaced where that assumption comes from and what it can't catch. Second, and more consequentially: asked whether categories should be app-first and nestable (`DISCORD:KEY:...`) so users could add arbitrary things later — which directly produced **protocol v3** (section 2.3). The resulting rule (category names the *mechanism*, not the app) is a better design than what the AI had originally specified, and it also happens to be what makes the future config-editor idea viable.
- **The student empirically corrected the AI.** Told that Windows shortcuts are always "modifiers + exactly one key" and that `CTRL+N+P` therefore couldn't be configured anywhere, they went and tested it in Discord and reported that it *was* recordable. Correct — Discord does its own key-state tracking rather than using Windows' registered-hotkey API. Checking a confident claim against reality instead of accepting it is exactly the habit worth documenting for the reflection.
- **Naming and comments were actively negotiated, not accepted by default** — `modifiers` was rejected as unclear and replaced with `heldKeys`/`tappedKey`; the student asked what a *decent* comment on `KEY_NAMES` would be rather than accepting a generated one. Consistent with the section 10 conventions being treated as a live standard rather than a one-off decision.

**Added 2-8-2026 (`SHELL:` category + Discord migration session):**

- **The `SHELL:` handler was written by the student**, same incremental pattern as the rest of the companion app: the pieces and the pitfalls were described, the student wrote it. Submitted correct on the first pass, including two improvements suggested in passing that were applied without prompting (moving `split("+")` inside the `KEY:` branch so it doesn't run for every category, and adding the `else` that reports unknown categories). The first Python contribution this project that arrived without a debugging round — a visible difference from the 1-8-2026 session's list of bugs.
- **The student caught an unsourced claim — third time in this log.** Asked "where did you get camera from?" about a `CTRL+SHIFT+V` default the AI had stated confidently; it was a guess pattern-matched off the real `CTRL+SHIFT+M`/`D` family, not a Discord default. Follows the same shape as the earlier `CTRL+N+P` correction (1-8-2026). At this point it's a **habit rather than an incident**, and it's the single most portfolio-worthy behaviour in this log: the student verified against Discord's own dropdown rather than against the AI's list, and ended the session with six keybinds all confirmed on hardware instead of six assumed from a plausible-sounding table.
- **Every `SHELL:` command was tested in `cmd` before becoming a button.** Deliberate layer isolation — if the string fails in a terminal it will fail from Python, and testing it there separates "wrong command" from "broken pipeline." Same instinct as the display-before-touch-before-LVGL sequencing from the very start of the project, applied to a completely different domain. This paid off directly: the Claude app ID was proven working in `cmd` before it ever had to survive C string escaping.
- **A design decision was explicitly delegated, and that was the right call.** After receiving the trade-offs on dict-vs-`if`/`elif` for category dispatch, the student said "make a decision please" rather than continuing to deliberate. Worth recording honestly next to the times they pushed back: recognising when further analysis stops paying — on a reversible, low-stakes structural choice — is its own judgement, not an absence of one. (The decision and its reasoning are in 8.3.)
- **Portability was raised unprompted** ("is this for everyone who installs VS Code?"), which surfaced that *none* of the `SHELL:` strings and three of the Discord keybinds are portable, and turned roadmap item 9 from an abstract nice-to-have into a motivated feature with a concrete problem behind it (see 8.5). Second time this session the student has thought about users other than themselves — consistent with the 26-7-2026 "how would someone less tech-savvy edit this?" note.
- **A feature was cut on evidence rather than kept out of sunk cost.** The camera button was tested, found to require picking a background every time, and dropped — with the reasoning stated as "a button that only gets you to a dialog is worse than no button." The generalisation that followed (8.4: `KEY:` can't drive anything needing a decision after the trigger) came *from* that concrete failure rather than from theorising, which is the right direction of travel.
- **AI-authored code in this session:** none of substance. The firmware changes were string edits to data tables; the Python was the student's. This session sits at the opposite end of the scale from 26-7-2026's prototype rush — worth noting for an honest reflection that the balance shifts by session and by task type, rather than trending monotonically in one direction.

**Added 3-8-2026 (Spotify / SMTC session):**

- **The student caught a false AI claim that had already been written into this log.** Asked "will the IDs always be the same for my device and also for other people's devices?" — which prompted an actual check and revealed the "per-install hash" claim in 4.20 was wrong (the publisher hash is deterministic across all machines). This is the **fourth** unsourced or incorrect AI claim caught in two days, after the `CTRL+N+P` Discord shortcut, the `CTRL+SHIFT+V` camera "default", and an overstatement that a featured artist appearing in the title string meant the metadata "wasn't lost". Worth stating plainly for the portfolio: the corrections came from the student asking a clarifying question about scope, not from the AI self-checking. The log now carries a visible correction rather than a quiet edit, which is the honest way to handle it.
- **Pushed back on a rationalisation.** When told the featured artist was "right there" inside the title string, replied *"that is just the song name pal"* — correctly rejecting text-inside-a-display-field as equivalent to structured data. The AI conceded and the limitation was recorded accurately in 8.6 instead of being explained away.
- **Asked "what's the purpose of this?" about async** rather than copying the pattern and moving on. The honest answer — that async buys nothing in a single-task script and is a shape imposed by WinRT rather than a feature being exploited — was more useful than a generic concurrency explanation, and only surfaced because the question was asked directly.
- **Asked for the same explanation from a different angle** when the first one half-landed, rather than pretending to understand. Then flagged explicitly that async will remain a weak point and asked for it to be revisited as the project continues. Knowing what you don't know, and saying so, is worth recording.
- **Every step was verified before building on it:** enumerate sessions → read metadata → attempt control. The `try_skip_next_async()` test was the load-bearing one — it decided whether SMTC could replace the Web API for control or only for display. Same layer-by-layer discipline as the display bring-up and the `cmd` testing in the previous session.
- **Chose the lower-hassle architecture on its merits**, explicitly reasoning that SMTC is "more plug and play" than an approach requiring manual account allowlisting. That instinct turned out to be correct for stronger reasons than were initially visible (Premium gating, 5-user cap, token expiry). Then accepted a real functional loss (Like, full artist list) rather than reintroducing the whole OAuth stack for one button — a proportionate trade rather than completionism.
- **Two bugs in one line** (4.25) traced back to a habit already flagged in the previous session's code review (reassigning a loop variable to a value derived from itself). Harmless in the `KEY:` loop, destructive here. Worth noting that a style comment made once and not acted on resurfaced as a real bug — an argument for treating those notes as more than preference.
- **AI-authored code this session:** the enumeration script (pure API boilerplate, given directly) and the skeleton of `function_find_spotify` with TODOs. The student wrote both TODO bodies and hit three bugs doing so, all of which were found by reading the code rather than by running it. Consistent with the established balance: boilerplate handed over, logic left to the student.

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

*Status 1-8-2026: the firmware follows these conventions. The Python companion app follows them in spirit — its comments explain non-obvious "why" (why `split(":", 1)` takes a limit, why empty lines appear, why held and tapped keys are separated), not what the syntax already says. Naming in the Python file is currently mixed camelCase/snake_case (`baudRate` next to `resolvedKeys`); Python's own convention is snake_case for variables and ALL_CAPS for constants. Worth aligning eventually, not urgent. Note C/C++ has no single equivalent convention — the firmware mixes the student's camelCase with LVGL/TFT_eSPI's snake_case unavoidably, and that's fine as long as the project's own code is internally consistent.*

---

## 11. C Concepts Reference (introduced in the multi-page build — study material)

*Written 26-7-2026, extended 1-8-2026. `user_data` and pointer theory were **deliberately parked** on 1-8-2026 — the working model below is enough to read the firmware fluently; the rest can wait until something forces it. When it does click, rewrite this section in your own words (rewritten explanations stick better than read ones).*

**Working model that suffices for now:** a pointer is a *handle to a thing that lives somewhere else*. `btn` is a handle to a button, `def` is a handle to a table row, `page` is a handle to a page description. Pass handles around; read from them with `->`. That covers ~90% of the pointer usage in the firmware, and it's the same instinct that already worked fine for weeks with `lv_obj_t *btn`.

### `user_data` and `void *` — the coat check

`lv_obj_add_event_cb(btn, actionButtonEvent, LV_EVENT_CLICKED, (void *)def)` works like a coat check:

1. **Check-in** (in `buildPage`): the last argument hands LVGL "a thing" to store with this specific button — here, a pointer to this button's own row in the data table.
2. **Storage:** LVGL keeps it as `void *` — pointer to *something*, type deliberately erased. LVGL is generic and can't know `ButtonDef` exists. The clerk stores coats, bags, umbrellas — same shelf, no questions asked.
3. **Pick-up** (in the callback): `lv_event_get_user_data(e)` returns the exact same pointer, still as `void *`. The cast `(const ButtonDef *)` is you saying "unwrap this as what I know I checked in."

**Why it exists:** `actionButtonEvent` is ONE function shared by ~30 buttons. When it fires it must answer "which button was I?" — the answer is whatever was checked in with that widget. Each button carries a pointer to its own identity. Without this: 30 separate callback functions.

**Full trace of one button's life:** boot → `buildPage` reaches `i=1` of `spotifyButtons` → `def` points at that row → the pointer is checked in with the newly created button. Later: tap → LVGL finds that widget's stored callback + pointer → calls `actionButtonEvent` → cast unwraps → `def->action` → `Serial.println(...)`. The table row and the on-screen button are permanently linked by one stored pointer.

**Alternatives that were considered and why this won:** a function pointer stored in `ButtonDef` instead of an action string would avoid `void *` entirely, but needs ~30 near-identical functions and puts each button's behaviour far from its label. Decisive argument: an action string is *data*, so it can come from a config file pushed from the PC — a function pointer cannot. Protocol v3 (2.3) leans on the same property.

### `*` and `&` are opposites

- `&x` — **take** an address ("address of x")
- `*p` — **follow** an address ("the value p points at")
- In a *declaration*, `int *p` — the `*` is part of the type, not an operation
- `p->field` is shorthand for `(*p).field` — follow the pointer, then take the field

Three places in the firmware, all the same idea:
```cpp
buildPage(&pages[i]);                     // & : hand over an address
const ButtonDef *def = &page->buttons[i]; // & : take an address
def->action                               // -> : follow an address
```

**Why a pointer rather than a copy** for `def`: LVGL keeps it for the lifetime of the program. A copy would be a local variable that dies when the loop pass ends, leaving LVGL holding the address of something gone. The `const` tables live in flash and never move, so the address stays valid forever.

**Copies are the default in C; pointers are the opt-in** when a copy can't do the job — because something must outlive the current scope, because a function needs to hand back more than one value (`tft.getTouch(&x, &y)`), or because copying a big struct is wasteful.

### Structs and arrays of structs

A `struct` bundles variables under one name (`ButtonDef` = label + action + target traveling together). `.` accesses fields on a value, `->` through a pointer — same rule as `data->state` in the touch callback.

**`PageDef`'s `buttons` field is a pointer to an array, not a copy of it** — writing an array's bare name in C gives the address of its first element (which is why it's `spotifyButtons`, not `&spotifyButtons`). `PageDef` is a page's *description*, like a library index card: it doesn't contain the book, it says which shelf. That's also why `buttonCount` must sit right beside it — **a pointer carries no length information**, so a wrong count either hides buttons or walks off the end of the array into garbage. The planned `COUNT(arr)` macro exists to make that impossible.

### Function pointers

`makeBarButton(..., lv_event_cb_t callback, ...)` — a parameter holding *which function to call*. These were in use all along without the name: passing `backButtonEvent` to `lv_obj_add_event_cb` passes the function itself as a value (no parentheses — with them it would be a *call*). The helper just makes it explicit: the caller picks the callback the same way it picks the x-position.

### Callbacks and inverted control flow

`lv_obj_add_event_cb` does nothing visible when called — it *stores an arrangement for later*. That's what makes it feel slippery compared to `lv_obj_set_size`, which acts immediately.

Normally your code calls the library. With callbacks it's reversed: **the library calls you.** `actionButtonEvent` appears in the file but no line anywhere calls it — searching for `actionButtonEvent(` finds only the registration. LVGL does the calling, from inside `lv_timer_handler()`. The same pattern was already in use three times: `lv_display_set_flush_cb`, `lv_indev_set_read_cb`, and now `lv_obj_add_event_cb`.

This is precisely why 4.16 was confusing: registration and invocation are separate things, and you can have a perfectly correct registration while invocation is completely dead.

### LVGL vocabulary (clarified 1-8-2026)

- **`lv_tick_inc()` tells LVGL what time it is; `lv_timer_handler()` gives it permission to act on that.** Both required. 4.16 was having the second without the first.
- `lv_timer_handler()` is not specifically "check touch and redraw" — it runs *any internal timer now due*. Currently: the input timer (~30ms — calls `my_touch_read`, interprets press/release transitions, fires event callbacks) and the refresh timer (~33ms — repaints dirty areas). With `delay(5)` most calls find nothing due and return immediately; that's normal and cheap, not waste.
- **"Dirty"** = a screen region no longer matching what should be displayed. Widgets mark *themselves* dirty when something changes (LVGL never diffs the screen); the refresh timer then repaints only those rectangles. Vocabulary chain: **invalidate** (mark) → **dirty area** (rectangle awaiting repaint) → **refresh** (timer processing the list) → **flush** (your callback pushing pixels to hardware). This is also why a ~19KB buffer suffices for a 300KB screen — and why 4.15's `lv_obj_invalidate()` attempt did nothing: the area was already marked, nobody was reading the list.
- **A screen is just a widget with no parent** — `lv_obj_create(NULL)`, not a special type. `lv_screen_load()` swaps *which tree is active*: nothing is created or destroyed, the whole new screen is marked dirty, and touch now hit-tests against the new tree. The other 8 screens still exist in RAM, complete but unreachable.
- **Parent/child determines coordinates.** A label created with `btn` as parent centers *inside the button*; the same label created with `scr` as parent would center on the whole screen. Two similarly-named things that differ: `LV_TEXT_ALIGN_CENTER` centers the *lines of text* against each other inside a label; `lv_obj_center()` centers the *label widget* inside its parent.

### Smaller ones

- `LV_SYMBOL_COPY "\nCopy"` — adjacent string literals are glued into one string at compile time; the symbol macros are themselves just short strings (special font characters), yielding icon + newline + text in one label.
- `i % gridCols` / `i / gridCols` — recover column and row from a flat index (the inverse of the old nested row/col loops), feeding into the familiar `start + index * stride`.
- `sizeof(pages) / sizeof(pages[0])` — element count of an array (total bytes / bytes per element); never goes stale when rows are added.
- `NULL` as sentinel — one field doing double duty ("no action" *means* "this is a navigation button").

### Python equivalents worth noting (1-8-2026)

- **Truthiness:** `if not line` doesn't ask "does the variable exist" — it asks "is this value *falsy*." Empty bytes `b''`, empty string, empty list, and `0` are all falsy; anything with content is truthy. C has no equivalent for strings/arrays, only for zero. Note `b" "` (a space) is truthy, which is why stripping before checking catches whitespace-only lines too.
- **Unpacking is strict:** `a, b = some_list` needs exactly two items or it raises `ValueError`. That's the real reason the colon guard exists — `"HELLO".split(":", 1)` succeeds and returns one item; it's the *unpack* that fails.
- **`print()` returns `None`** — it displays, it doesn't produce a value. `x = print(y)` silently stores nothing.
- **Slicing:** `keys[:-1]` is everything but the last, `keys[-1]` is the last. Negative indices count from the end — handy because the tapped key is always last regardless of how many modifiers precede it.
- **`for name in names` walks items directly**, not indices — unlike C's `for (int i = 0; ...)`. `enumerate()` gives both when the position is needed.

---

## 12. Immediate Next Steps for Next Session

*(as of 3-8-2026, 01:06)*

**Done this session:** Spotify architecture changed from `spotipy` to Windows SMTC (2.5) — the single biggest dependency reduction in the project. Reading and control both verified against desktop Spotify with no developer account, no Premium gate, no allowlist, no token expiry. Four new hiccups (4.23–4.26), one new architecture subsection (8.6), and **a correction to 4.20 / 8.5** where a previously logged AI claim about packaged-app IDs turned out to be false.

1. **Wire `SPOTIFY:` into the dispatcher.** First decide the async crossing (8.6): almost certainly `asyncio.run()` inside the handler for now. Then a second-level dispatch on the action (`PLAYPAUSE`, `NEXT`, `PREV`, `SHUFFLE`, `SEEKFWD`, `SEEKBACK`) — note this is the **fourth category**, which 8.3 named as the point to revisit the dispatch-table question.
2. **Rebuild the Spotify pages as one page** (2.5). Six buttons, suggested: Play/Pause, Previous, Next, Seek −10, Seek +10, Shuffle. Volume stays on the Media page as `KEY:MEDIA_*` — SMTC has no volume concept. This deletes the `PAGE_SPOTIFY2`/`PAGE_SPOTIFY3` chain, the `prevPage`/`nextPage` links and the "n/m" titles, removing the last placeholders from the firmware **and** most of the fiddly part of the page table.
3. **Seek is more code than it looks.** There is no relative "skip 10 seconds" call — read the current position from `get_timeline_properties()`, add or subtract, then call `try_change_playback_position_async()` with an **absolute** target in **ticks (100-nanosecond units)**, not seconds. Budget for this being the awkward one.
4. **Handle "Spotify isn't running."** `function_find_spotify` already returns `None`; the dispatcher needs to do something sensible with that rather than crash. Same shape as the existing `if None in resolvedKeys` guard.
5. **`COUNT(arr)` macro** — still outstanding from the last session, and item 2 above changes button counts again, so it will bite a third time if skipped. `#define COUNT(a) (sizeof(a) / sizeof((a)[0]))`.
6. **Comment the machine-specific strings** (8.5) — reduced in scope now that the Claude app ID turns out to be portable, but the username path, PATH dependency and self-assigned Discord keybinds still warrant a note.
7. **Decide whether Like earns the Web API.** The only remaining reason to touch `spotipy` at all. It would mean the full OAuth stack, the Premium gate and the 5-user allowlist for **one button**. Defensible either way — but worth deciding deliberately rather than drifting into it.
8. **OBS** (`obsws-python`) — the remaining integration with no shortcut available. First action string carrying a real parameter (`OBS:SCENE:1`), which is exactly what 8.4 identifies as the thing keybinds can't do.
9. **Two-way protocol** (roadmap item 6) — now much better motivated: SMTC already exposes `title`, `artist`, `is_shuffle_active` and `auto_repeat_mode`, so the data for a now-playing display and for real state on the shuffle/repeat buttons is **already available and proven readable**. Only the PC→ESP32 direction is missing. This is also where async stops being ceremony and starts earning its keep (8.6).
10. **Async remains a known weak point** — flagged explicitly by the student at the end of this session. Worth building the intuition incrementally as items 1 and 9 come up, rather than in one lump.
11. **Python tidy-up** (optional): extract the `KEY:` branch body into `handle_key()`; align naming to snake_case; auto-detect the COM port via the CP2102's USB VID/PID (`0x10C4`/`0xEA60`) instead of hardcoding COM13; wrap the port open in a retry so unplugging doesn't kill the script.
12. **Parked, unchanged:** PTT / hold-state would need `KEYDOWN:`/`KEYUP:` (8.4); sequential combos would need a `;` separator (8.3); Discord RPC only after item 9 exists (2.4); the ESP32-S2 mini could be retested, ideally with the VBUS/GND multimeter check that was never actually done.