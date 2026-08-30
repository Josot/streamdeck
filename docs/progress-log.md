# StreamDeck Project — Progress Log

**Purpose of this document:** full context dump for continuing this project in a future chat, and source material for a later portfolio item. Written in enough detail that someone (or an AI) with zero prior context could pick this up and continue.

**Last updated:** 21-8-2026 (second session that day — see the 4.39–4.41 hiccups and the section 12 rewrite)

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

> **Correction (17-8-2026):** the "Spotify has no push mechanism" claim was true of the **Web API**, not of Spotify as such. SMTC exposes three change events on the session object — `MediaPropertiesChanged`, `PlaybackInfoChanged`, `TimelinePropertiesChanged` — so track changes, play/pause state and shuffle state can all arrive as callbacks rather than being polled for. This materially changes roadmap item 6: a now-playing display can be event-driven. Second time a stated constraint has turned out to be Web-API-specific rather than Spotify-specific (cf. 2.5). Events remain **unexplored** — noted, not tested.

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
| `SPOTIFY:` | Windows SMTC media session (decided 3-8-2026, see 2.5 — was: Spotify Web API via spotipy). **Wired into the dispatcher 17-8-2026.** |
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

> **Correction (17-8-2026) — the fourth bullet was never applied, and is now deliberately reversed.** The Spotify page still sends `SPOTIFY:PLAYPAUSE` / `NEXT` / `PREV`; transport never moved to `KEY:MEDIA_*`. Checked by grepping `spotifyButtons[]` rather than trusting the log — worth doing, because the log claimed the migration was complete while listing a consequence that hadn't happened. **And the bullet is now wrong on its merits:** it existed to avoid OAuth for things media keys handle anyway, but media keys broadcast to whatever Windows thinks is active, which was the known unreliability. SMTC targets Spotify by `source_app_user_model_id` at zero setup cost, so transport belongs under `SPOTIFY:` — which is what 2.5's six-button page already assumed. The Media page keeps its `KEY:MEDIA_*` buttons as the deliberate "whatever is playing" alternative.

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

### 2.6 Spotify volume: the Windows audio mixer (`pycaw`) — decided 17-8-2026, **not built, and now re-opened**

SMTC is a *media session* API and has no volume concept at all (2.5). The obvious remaining route was the Web API's volume endpoint — Premium, OAuth, allowlist, expiring token, for one number.

**Third option found: Windows' per-application audio mixer.** Every process gets its own volume node — the thing you see when you right-click the speaker icon. `pycaw` binds it: `AudioUtilities.GetAllSessions()`, match `session.Process.name()` against `"Spotify.exe"` (same find-by-name idea as the SMTC lookup), then `ISimpleAudioVolume` with `GetMasterVolume()` / `SetMasterVolume(float 0.0–1.0, None)`.

**Investigated and rejected: Last.fm and other third-party services.** Last.fm is scrobbling and metadata only — it has no playback control of anything. More generally, **no third party can offer "Spotify control" as a service**, because control requires the user's own OAuth token against Spotify's API; anything claiming otherwise is a wrapper that still makes you authenticate, with the same Premium gate plus a middleman. The 2.5 constraints are Spotify's, not an artefact of the client chosen.

**The resulting volume ledger:**

| Volume | Mechanism | Status |
|---|---|---|
| System volume | `KEY:MEDIA_VOLUP` / `VOLDOWN` / `MUTE` | ✅ working |
| Spotify's share of system audio | `pycaw` per-app mixer | decided, not built |
| Spotify's *own internal* slider | Web API only | not pursued |

**Differences from SMTC worth knowing before building it:** `pycaw` is **synchronous** — no coroutines, no `asyncio.run()`, making it the simplest integration in the project. It's COM rather than WinRT, so the naming is different but the docs situation is *better* (real Python examples exist, versus SMTC's translate-from-C# situation). Everything is read-modify-write; there is no toggle. And it has a different notion of "Spotify": SMTC finds a *media session*, `pycaw` finds an *audio session*, and these can disagree — Spotify paused for a while may have the former and not the latter.

**Cost accepted:** the mixer changes the app's output level, not Spotify's own slider, so the Spotify UI won't move. Functionally identical for listening; occasionally confusing when looking at the app. Third Windows-specific dependency, consistent with the trade already accepted in 2.5.

> **Re-opened 21-8-2026 (second session).** The student is no longer sold on `pycaw`, specifically because of the cost accepted in the paragraph above: *"pycaw might not really be the move... might use API way for that. To have it done proper with spotify volume slider."* That is the third row of the ledger — Spotify's own internal slider — which only the Web API reaches. Not a decision yet, and it changes the shape of 2.7: it would make **volume**, not Like, the feature that justifies the OAuth stack. Worth costing both before building either, since `pycaw` is an afternoon and the Web API is a dependency plus an auth flow. **No work done on volume this session; it is off the critical path until decided.**

### 2.7 Spotify Web API as a *second* backend — raised and parked 21-8-2026

Raised by the student unprompted: why not make the app work through **both** SMTC and the Spotify Web API, and let the user choose? Parked, but liked, and the reasoning is worth keeping because the instinct behind it is the 2.3 user-freedom argument applied one level up.

**Two different ideas were being run together, and they cost very different amounts:**

- **Cheap version — Web API only for what SMTC can't reach.** Not a "both backends" design at all. It is one more action string routed to a different mechanism, exactly the way `KEY:` and `SHELL:` already coexist. The dispatch table (8.9) supports it as-is: a `"LIKE"` row whose function happens to use `spotipy` instead of `winrt`. Nothing else in the app changes.
- **Expensive version — every action available through either backend, selectable at runtime.** Needs a common interface both implement, a selection mechanism, and a duplicate implementation of everything SMTC already does perfectly. Pays OAuth, the Premium gate, the 5-user allowlist, token refresh and network latency on a button press that is currently milliseconds — to reach the same play/pause that already works.

**The counter-argument to "just give the user the option," recorded because it is the interesting one.** `KEY:` and `SHELL:` are open-ended because the set of things users want is *unenumerable* — the user brings their own strings and no code is written per new use. The freedom is free. A second backend is not that: it is a second implementation to write and maintain so the user can pick a slower, auth-gated path to an identical outcome. And the choice on offer isn't a feature, it's plumbing — a choice most users cannot make an informed decision about, which usually means it should be a default rather than a setting. It also can't be expressed in a config file: the Web API path needs the user's own OAuth app registration or a slot on the 5-user allowlist, so the "option" is a setup procedure.

**Where the Web API genuinely wins, i.e. the trigger to un-park this:** Like/save state, playlist context (what is playing *from*), the queue, album art URLs, track features, and control when Spotify isn't running locally.

**Where it does *not* win, contrary to the motivation given ("current data"):** track name and position. The Web API's `currently-playing` is polled, so its position is as stale as SMTC's and arguably worse — SMTC at least hands back `last_updated_time` to correct against (8.7), where the Web API returns whatever the server knew when asked, plus a network round-trip. And per the correction at the top of section 2, SMTC has **change events**, so a now-playing display can be push-driven locally with no polling at all.

**Parked with a named trigger:** revisit when a feature SMTC cannot reach is actually wanted — Like, playlists, queue, album art — not for position or track metadata. Doing it for **Like alone** would also answer next-steps item 7 and buy the OAuth experience without duplicating a single working function.

### 2.8 Album art on the display — investigated and parked 21-8-2026

Asked whether SMTC can supply the album cover. It can, and the SMTC end is the *smallest* part of the job.

- **SMTC side:** `try_get_media_properties_async()` exposes a `thumbnail` property, which is a `RandomAccessStreamReference` — not bytes, not a URL. It needs `open_read_async()` and then a WinRT buffer read to get actual bytes. ⚠️ **This is from memory of the WinRT surface and has not been tested** — check `GlobalSystemMediaTransportControlsSessionMediaProperties.Thumbnail` in Microsoft's C# reference and translate, per the standing rule in 2.5.
- **The real cost is downstream.** Whatever comes back is a JPEG or PNG of unknown size, and the ILI9488 decodes neither. The pipeline is: WinRT stream → bytes → decode (Pillow) → resize → convert to RGB565 → push over serial → into a display buffer that has already overflowed DRAM once (4.12).
- **Rough numbers:** a 100×100 thumbnail at RGB565 is ~20 KB. At 115200 baud that's ~11 KB/s, so roughly **two seconds per cover**, blocking the serial line throughout.
- **The Web API alternative doesn't avoid this** — it hands you a URL, but the fetch, decode, resize and convert are identical PC-side work. The SMTC awkwardness is only the first third either way.

**Parked behind roadmap item 6**, which it strictly depends on: it needs the return path to exist, plus a binary transfer mode alongside the line-based text protocol, plus a fresh look at the memory budget. Not a feature — a project.

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

### 4.27 Calling a coroutine without awaiting it — twice in one session (17-8-2026)

The single most repeated mistake of this session, and the one that best illustrates the async model.

- **First instance:** `session = function_find_spotify()` inside the `SPOTIFY:` branch of `main.py`'s blocking `while True` loop. Calling an `async def` **does not run its body** — it builds a coroutine object and hands it back, unstarted. So `session` held a coroutine, which is not `None`, so the `if session is None` guard passed unconditionally: Spotify could have been closed entirely and the check would not have noticed.
- **Second instance:** `return session.try_change_shuffle_active_async(...)` inside `function_toggle_shuffle` — missing `await` on the return line, so the SMTC call was constructed and never made.
- **The symptom to recognise:** `RuntimeWarning: coroutine 'x' was never awaited`. That warning *is* this bug.
- **Why the first one couldn't be fixed with a one-word patch:** the `while True` loop is ordinary sync code and cannot `await` at all. Wrapping just the lookup in `asyncio.run()` would have worked for one line and failed on the next, since the actual playback call is *also* a coroutine. The fix was structural — move the whole branch body into `async def function_handle_spotify(action)` and have the loop call `asyncio.run(function_handle_spotify(action))` once. All awaits then live inside one event loop per press.
- **Also caught in the test harness:** `asyncio.wait(1)` used where a sleep was meant. `asyncio.wait` takes a set of awaitables, not an int, and is itself a coroutine — so it did nothing, twice over. `await asyncio.sleep(1)` inside a coroutine, `time.sleep(1)` outside one.

### 4.28 Reading an attribute off the wrong object (17-8-2026)

`props.position` → `AttributeError`. `props` is `...SessionMediaProperties` (title, artist, album); position lives on `...SessionTimelineProperties`, from a separate `session.get_timeline_properties()` call. Also hit in the same family: `get_playback_info().controls` written without a receiver, as though it were a free function.

**Useful habit that came out of it:** the `AttributeError` message names the class it failed on. Reading that class name tells you whether you have the *wrong attribute* or the *wrong object* — here, unambiguously the second.

### 4.29 Comparing a WinRT enum to a string fails silently (17-8-2026)

`if info.playback_status == "PLAYING":` compiles, runs, raises nothing, and is **always false**. The drift correction therefore never applied and `corrected` silently equalled `reported` — no error, no clue, just a feature quietly not working.

- **Diagnosis path:** `print(info.playback_status)` was unhelpful (printed the enum *class*, indistinguishable from `type()` output). `repr()` gave the real member: `<...PlaybackStatus.PLAYING: 4>`, and `list(type(status))` gave the full set — CLOSED 0, OPENED 1, CHANGING 2, STOPPED 3, PLAYING 4, PAUSED 5.
- **Lesson worth keeping: reach for `repr()` when a `print()` looks uninformative.** `print` uses the human-readable form, which for enums can be lossy.
- **Fix:** import the enum and compare against a member:
  ```python
  from winrt.windows.media.control import (
      GlobalSystemMediaTransportControlsSessionManager as MediaManager,
      GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus,
  )
  ...
  if info.playback_status == PlaybackStatus.PLAYING:
  ```
- **Why the import is necessary at all, in plain terms:** the status is not text, it's one of six fixed values, and the name `PLAYING` only exists inside the enum class. Without importing it there is no way to *name* the value you want to compare against — and comparing to a lookalike string is exactly the silent failure above. (The `as PlaybackStatus` alias exists because WinRT type names are absurdly long; see 8.7.)
- **Deliberately `== PLAYING` and not `!= PAUSED`:** six states exist and only one means the position is advancing. `!=` would wrongly apply the correction during CHANGING and OPENED.

### 4.30 Naive vs timezone-aware datetimes (17-8-2026)

`datetime.now() - timeline.last_updated_time` → `TypeError: can't subtract offset-naive and offset-aware datetimes`.

- **Naive** = a datetime with no timezone attached; **aware** = the same instant plus an offset. Python refuses to mix them rather than guessing, because the gap between "14:32" and "14:32 UTC" could be anything.
- WinRT always returns **aware UTC** (`+00:00`), on every machine, regardless of the user's location — it's a property of the API, not of where the PC is. So the fix is one argument: `datetime.datetime.now(datetime.timezone.utc)`.
- Checked explicitly during the session whether the machine simply *was* on UTC — it wasn't (CEST, UTC+2). Had the mismatch gone unnoticed, the arithmetic would have been off by two hours and every seek would have jumped to the end of the track.
- **Keep everything in UTC rather than sidestepping via local time:** matching both sides as naive local would work until a DST boundary.

### 4.31 A test harness that would break on import (17-8-2026)

`session = asyncio.run(function_find_spotify())` sat at module level in `spotify_test.py`. Harmless in a scratch script, and three separate objections applied at different times:

1. **Dead code** while `function_show_now_playing` did its own lookup and never read it.
2. **Two event loops** — session found in one `asyncio.run()`, used in another. Probably fine; WinRT objects carry threading context, and "probably" is how intermittent failures start.
3. **The one that actually matters: top-level code runs on import.** The moment this file becomes `spotify_functions.py` and `main.py` imports it, importing the module would spin up an event loop and go looking for Spotify — and cache a session that dies when Spotify restarts.

**Rule adopted:** test harnesses go inside an `async def` called once at the bottom, never at module level. Same shape the real handler needs anyway.

### 4.32 `else: return` swallowed every parameterless action (18-8-2026)

The parameter split in the handler was first written as `if ":" in action: action, params = action.split(":", 1)` followed by `else: return`. The `else` was meant to mean "no parameter here" and instead meant "stop". `PLAYPAUSE`, `NEXT`, `PREV` and `SHUFFLE` all arrive without a colon, so **every single parameterless action returned before doing anything** — no error, no output, no traceback. Caught in review before it ran. Setting `params = None` on the line above makes the `else` unnecessary entirely; the `if` is then a pure enrichment step, not a branch.

### 4.33 A guard that printed but did not stop (18-8-2026 → 21-8-2026, three passes)

`if action not in SPOTIFY_FUNCTIONS: print(f"action not recognised: {action}")` — with no `return`. `print` does not halt execution, so the lines below ran regardless and `SPOTIFY_FUNCTIONS[action]` raised `KeyError` on the very value the guard had just rejected.

**The blast radius is the point, and it applies to any unhandled exception here.** `main.py`'s dispatch is `asyncio.run(...)` inside a bare `while True` with no `try`/`except`. An exception propagates straight out of the loop and **kills the whole companion app** — Discord keybinds, `SHELL:` buttons, everything — because one Spotify action string was mistyped. One misbehaving branch takes down four categories. This motivated next-steps item 4.

### 4.34 A module-level dict referencing functions defined below it (18-8-2026)

`SPOTIFY_FUNCTIONS = {...}` was first placed above the function definitions, near the imports where a constant "belongs". A module executes top to bottom, and a dict literal evaluates every name inside it *when the literal runs* — so referencing `function_toggle_shuffle` at line 10 is a `NameError` at import. It went unnoticed for a while because the dict was still empty. **Rule:** the table sits below the definitions and above the handler.

### 4.35 Renaming functions without updating the table (21-8-2026)

`function_seek_forward` → `function_seek_wrapper_forward`, but the dict still held the old names — `NameError` at import again, same mechanism as 4.34. Recorded separately because it is the specific, recurring cost of a dispatch table: **a rename now has two edit sites and nothing checks the link.** The consolation is the failure mode's timing — the dict is evaluated at import, so a stale name kills the app immediately and loudly rather than lying in wait until someone presses that one button.

### 4.36 `float(None)` and `float("")` take the entire app down (21-8-2026)

`SEEKFWD` sent with no parameter → `float(None)` → `TypeError`, unhandled, read loop dead (4.33's blast radius). The realistic inputs that do this:

| Input | `params` | Raises |
|---|---|---|
| `SEEKFWD` | `None` | `TypeError` |
| `SEEKFWD:` | `""` | `ValueError` |
| `SEEKFWD:1,5` (Dutch decimal comma) | `"1,5"` | `ValueError` |
| `SEEKFWD:1O` (letter O for zero) | `"1O"` | `ValueError` |

An `if params is None` check catches **only the first row**. Fixed with `try` / `except (ValueError, TypeError)` around the conversion, which covers all four in one guard. The general lesson: *you cannot test whether a string is a number by comparing it to things* — the set of bad values is infinite. Convert and catch the failure.

**Why this guard, when the seek clamp (8.7) was deliberately dropped.** The clamp guarded a *user action* that Spotify already handled correctly; this guards a *malformed action string* that nothing else handles. And the consequences differ by an order of magnitude: an unclamped seek produces a slightly surprising jump, while an unguarded conversion kills every button on the device. "Don't add defensive code without evidence" — the evidence here was running it and watching the traceback.

### 4.37 A bool parameter fed a string, failing silently (21-8-2026, caught in review, never ran)

An intermediate dict had `"SHUFFLE_DECIDE": function_set_shuffle`, whose second parameter is a **bool** passed straight to `try_change_shuffle_active_async`. Parameters arrive off the wire as **strings**, and every non-empty string is truthy — so `SHUFFLE_DECIDE:OFF` would have turned shuffle **ON**, with no error, no traceback and a plausible-looking result. **The most dangerous shape found this session**, precisely because nothing anywhere would have reported it. Fixed by `function_shuffle_wrapper`, which converts `"ON"`/`"OFF"` into real bools and rejects anything else. This is the concrete argument for the wrapper layer in 8.9: the table's job is to convert wire strings into Python types, and any entry pointing straight at a typed function is a silent bug waiting.

### 4.38 `{target:2f}` is valid syntax and wrong output (flagged 17-8, **not** deleted — see correction)

`print(f"seeking to target: {target:2f}")` — missing the dot. In a format spec, `2f` parses as **width 2**, not precision 2, so it printed six decimal places and never once raised. Flagged in three consecutive sessions and survived every time because nothing complained. **Category worth naming: the bug that produces wrong output through valid syntax is invisible to every tool and outlives several review passes.**

> **Correction (21-8-2026, second session).** This entry previously said the print was "resolved by deleting it entirely." **It was never deleted** — the line was still in `function_try_seek` at the start of the next session, found by reading the file rather than trusting the log. Second time a confident past-tense claim in this document turned out not to describe the code (cf. the "migration complete" note in 2.3, caught 17-8). The claim was written by the AI into the same log that already warned about exactly this.
>
> **And the print now stays, deliberately.** The student's call: *"I WANT the seeking to target to stay in there. I find it nice to have."* It is useful feedback on a button whose effect is otherwise invisible until you look at Spotify. The formatting bug is real and cosmetic — `{target:.2f}` fixes it whenever it is worth touching. Recorded as **a choice, not an oversight**, same as `return print(...)` in 8.9.

### 4.39 The handler was imported, wired, and never awaited (21-8-2026, second session)

`main.py`'s branch read `spotify_functions.function_handle_spotify_functions(action)`. That function is `async def`, so the call built a coroutine object and dropped it on the floor — **no error, no traceback, no button response**, just an eventual `RuntimeWarning: coroutine ... was never awaited` if Python got round to garbage-collecting it. Every Spotify press from the device did nothing.

Fixed with `asyncio.run(spotify_functions.function_handle_spotify_functions(action))` — the sync→async crossing, one event loop per press, which was the design settled in 8.6 all along. Telling detail: `import asyncio` was already at the top of `main.py` and otherwise unused, so the intent was there and only the call site was missing.

**Why it is recorded separately from 4.27** rather than as a repeat: 4.27 was a missing `await` *inside* async code, where the fix is one keyword. This is the boundary case — sync code calling into async — where `await` is not even legal and `asyncio.run()` is the only answer. Same symptom, different mechanism, and the boundary is the version that will recur (it is the same call shape OBS and any future category will need if they go async).

### 4.40 The test harness fired on import — exactly as 4.31 predicted (21-8-2026, second session)

4.31 described this failure in the abstract on 17-8. It then happened. The two lines at the bottom of `spotify_functions.py`:

```python
line = "SEEKFWD"
asyncio.run(function_handle_spotify_functions(line))
```

survived into the imported module, so **launching the companion app** printed, before the read loop had started:

```
current param: None does not work. For seek you need a number in seconds. e.g 10.5 or 4
```

It spun up an event loop, went looking for Spotify and ran a real seek attempt at import time. Harmless on the day only because the 4.36 guard was already there to catch `float(None)`; without it the app would have died on startup, every time.

**Resolved by deleting both lines.** The module is exercised through `main.py` now, so the harness had no remaining job. `if __name__ == "__main__":` was covered as the general mechanism — Python sets `__name__` to `"__main__"` on direct run and to the module name on import, so a block under that guard runs only when the file is the entry point. Worth having for the *next* scratch harness (repeat, `pycaw`); it was the wrong answer for this one, which was disposable.

### 4.41 Three bugs in the rebuilt Spotify page, all caught in review (21-8-2026, second session)

The page rebuild was written by the student and reviewed before flashing. None of these reached hardware:

| Bug | Effect if flashed |
|---|---|
| `"SPOTFY:SEEKBACK:10"` — missing `I` | Category never matches; falls to `main.py`'s `else` and prints `cannot find the given category: SPOTFY`. Silent from the device's side. |
| `"SPOTIFY:SEEKBACK:-10"` — sign in the string | `function_seek_wrapper_backward` already negates (`-float(offset)`), so `-(-10)` = **+10** and the back button seeks *forward*. |
| `LV_SYMBOL_VOLUME_MAX` / `_MID` / `_OK` on the seek and shuffle buttons | Icons left over from the deleted Vol+/Vol−/Like row — the same label-doesn't-match-action rot that made the old placeholder page 3 unreadable. |

The middle one is the interesting one and it is 8.9's wrapper rule seen from the other side: **the wrapper owns the direction, so the action string must carry magnitude only.** Putting the sign in the string means two places now encode it, and they cancel. Any future parameter with a convention baked into its wrapper has the same trap.

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
    ├── main.py              (serial read loop + top-level category dispatch: KEY / SHELL / SPOTIFY)
    ├── spotify_functions.py (SMTC functions, wrappers, SPOTIFY_FUNCTIONS table, second-level handler — 21-8-2026)
    └── spotify_test.py      (scratch pad — new SMTC work gets proven here before it moves)
```

**File-level split as of 21-8-2026** (see 8.9): `main.py` owns the serial loop and the four top-level categories; `spotify_functions.py` owns everything SMTC. The `KEY:` and `SHELL:` handlers stay in `main.py` — one is a dict lookup plus a keypress, the other is one line, and neither brings its own dependency or async model. This is the split 8.8 argued for: extract on merit, not to mirror the wire protocol.

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
| 3f | LVGL: multi-page navigation (home → category pages → paged categories) | ✅ Done (26-7-2026) — originally 9 pages incl. a 3-page Spotify chain, collapsed to 7 when Spotify became one page, then **back to 9 with the OBS build**: a 2-page OBS chain plus a Scenes page. **The OBS build exercised both link mechanisms at once for the first time** — prev/next between OBS 1/2 and 2/2 (siblings), and parent/back from Scenes to OBS (genuine child). See 8.1 for the `parentPage` convention that had to be restated. |
| 4 | USB HID | ❌ **Dropped as a requirement** — companion app handles keystroke simulation instead (see section 2) |
| 5 | Wire button grid → serial | ✅ **Done (1-8-2026)** — action strings confirmed arriving in the Python app; protocol upgraded to v3 (see 2.3) |
| 6 | Two-way serial protocol (status pushes back to display: track info, scene state, mute state) | ⬜ Not started |
| 7a | Python companion app — keystroke dispatcher | ✅ **Done (1-8-2026)** — reads serial, parses `CATEGORY:ACTION`, resolves arbitrary key combos, executes them. Keybinds page and Media page both confirmed working end-to-end on hardware. **Written by the student**, incrementally. |
| 7b | `SHELL:` category — Python handler **and** firmware action strings | ✅ **Done (2-8-2026)** — `subprocess.Popen(action, shell=True)` on the Python side, five System-page buttons on the firmware side (Lock, Sleep, Notepad, Claude, VS Code). Lock and Sleep both confirmed on hardware; see 4.19–4.21 for the Windows-specific gotchas. |
| 7c | Python companion app — Spotify | ✅ **Done for the built function set (21-8-2026, second session).** `spotify_functions.py` is imported *and awaited* by `main.py` (4.39), and **all six device buttons are verified working on hardware**: Play/Pause, Previous, Next, Seek −10, Seek +10, Shuffle. The firmware and the `SPOTIFY_FUNCTIONS` keys now agree exactly. `SHOWDATA` works from the app but has no button (it returns metadata the dispatcher discards — real at roadmap item 6). Deliberately **not** built: repeat (deprioritised by the student), volume (2.6 re-opened, Web API vs `pycaw` undecided). |
| 7e | Spotify volume via `pycaw` (per-app audio mixer) | ⬜ Not started — decided 17-8-2026, see 2.6. Sync, no auth, simplest integration left. |
| 7d | Discord — migrate to `KEY:` global keybinds | ✅ **Done (2-8-2026)** — 6 buttons, all verified on hardware with Discord unfocused. No API needed; see 2.4 for why, and 8.4 for what keybinds can't reach. |
| 8 | Python companion app — OBS (`obsws-python`) | ✅ **Done (21-8-2026, second session)** — `obs_functions.py` built and wired; see 8.10. Seven table entries across three firmware pages: `SCENE` (5 scenes), `STREAM`, `RECORD`, `RECORDPAUSE`, `MUTE` (mic + desktop), `HIDETOGGLE`, `CLIP`. Scene switching verified end-to-end from the touchscreen. ⚠️ `RECORDPAUSE` cannot work until OBS's Recording Quality is changed off "Same as stream", and `CLIP` needs the replay buffer running — both are OBS settings, not code. |
| 9 | Polish (icons, config format, case, etc.) | ⬜ Not started — roadmap idea: long-term, a layout editor in the companion app (device stores a layout pushed from the PC) so button changes never require compiling firmware; see section 12 |

**Toolchain/infra status (all done, not part of the numbered roadmap but consumed real time):**
- ✅ Arduino IDE toolchain fully working on classic ESP32 (superseded by PlatformIO but was the original proof-of-concept environment)
- ✅ Switched to PlatformIO + VS Code, fully working build/upload/monitor cycle
- ✅ Git version control set up (command line), repo published to GitHub
- ✅ LVGL 9.5.0 installed and wired to TFT_eSPI (flush + touch read callbacks), DRAM budget fixed
- ✅ `lv_tick_inc()` wired in `loop()` — mandatory for LVGL timers on Arduino (see 4.16)
- ✅ Python environment working (`pyserial` + `pynput` + the four `winrt-*` packages from 4.24); full touchscreen → ESP32 → USB → Python → real keystroke pipeline proven on hardware, and as of 17-8-2026 the same pipeline through to real Spotify playback control

---

## 8. Current Working Code (as of this log)

**Full current firmware and companion app: see git, as of 17-8-2026** (`firmware/src/main.cpp`, `companion-app/`). Older versions live in git history — no longer embedded here.

**Where the Spotify code currently lives (17-8-2026), because it is split across two files:**
- `main.py` — has the `SPOTIFY:` branch, `function_find_spotify`, `function_handle_spotify`, and transport (PLAYPAUSE / NEXT / PREV). Working on hardware, committed.
- `spotify_test.py` — has `function_get_time_position`, `function_try_seek`, `function_set_shuffle`, `function_toggle_shuffle`. All tested and working, **none of it dispatched yet**. Merging these into `spotify_functions.py` is next-steps item 1.

The firmware is unchanged since 3-8-2026 — no reflash was needed for any of today's work, because the Spotify page was already sending the right action strings.

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
- ✅ **`COUNT(arr)` is in (21-8-2026, second session), after being outstanding for five.** `#define COUNT(a) (sizeof(a) / sizeof((a)[0]))`, declared at the top of the data section, used for every row of `pages[]` and for `pageCount` itself. The `buttonCount` field still exists and `buildPage` is unchanged — the only difference is that the number is now computed by the compiler instead of typed by hand, so it cannot drift from the array. **The `pages[]` `buttonCount` had previously been forgotten twice in one session** (System and Discord), which was the single most repeated mistake in the project.
  - **Why it must be a macro, not a function:** `sizeof` needs the real array type in scope. Inside `buildPage`, `page->buttons` is a `const ButtonDef *` — the length is gone, and `COUNT(page->buttons)` would compile happily and evaluate to `0`. It only works at the `pages[]` table, where `spotifyButtons` is still visibly an array.
  - The inner parens in `(a)[0]` and the outer parens around the whole expression are the standard macro-hygiene reflex: a macro is textual substitution, so without them a caller's surrounding operators can regroup the expansion.

- **The OBS build (21-8-2026, second session) put the page count back to 9** and used both linking mechanisms in one category for the first time: `PAGE_OBS` ↔ `PAGE_OBS2` as a prev/next sibling chain, and `PAGE_SCENES` hanging off `PAGE_OBS` as a genuine child. **`PAGE_SCENES` is also the first page mixing a nav button with action buttons** — `buildPage` needed no change, because it already branches per button on `action != NULL`.
  - **The `parentPage` convention had to be restated.** Every earlier page used `PAGE_HOME` as parent, which read like a rule but was really an artefact: chained siblings have no meaningful parent *among themselves*, so home was the only sensible target. Scenes is a real child, so its parent is `PAGE_OBS`. Stated properly: **back goes to the logical parent; chained siblings use home because they have none.**
  - ⚠️ **The enum↔`pages[]` correspondence is still completely unguarded, and it broke once this session.** A paste dropped `sceneButtons[]` and its `pages[]` row while leaving `PAGE_SCENES` in the enum — 9 enum entries against 8 array rows. Result: Scenes would have opened Media, Media would have opened System, and System would have indexed `screens[8]` **one past the end of an 8-element array**. Caught in review, never flashed. Worth being explicit that **`COUNT` does not help here**: it guards `buttonCount` against its own array, and nothing checks that the enum and `pages[]` have the same length or order. That correspondence is still held by hand.

- **Spotify collapsed from three pages to one (21-8-2026, second session).** `spotify2Buttons[]`, `spotify3Buttons[]`, `PAGE_SPOTIFY2` and `PAGE_SPOTIFY3` all deleted; the "n/m" titles are gone and the surviving page's `nextPage` is back to `-1`. **This was the project's first array/page *deletion*** — the direction 8.2's ordering trap actually punishes, since removing two rows from the middle shifts `PAGE_OBS`, `PAGE_MEDIA` and `PAGE_SYSTEM` down by two. Enum and `pages[]` were edited in lockstep and it landed first try. The final page is Play/Pause, Previous, Next, Seek −10, Seek +10, Shuffle — six buttons, one grid, everything on it actually reaching a function.

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

**`try` / `except` around the dispatch (added 21-8-2026, second session)** — next-steps item 4, closed. The `try` opens after the category/action split and wraps the whole `if`/`elif`/`else` chain; the `except` prints and lets the loop continue.

- **Deliberately `except Exception as e`, not a bare `except:`.** Python's error classes hang off `BaseException`, and `KeyboardInterrupt` and `SystemExit` sit *beside* `Exception` rather than under it. A bare except swallows Ctrl+C, so the handler would print "error, carry on", loop, and the script would become unkillable from the terminal. `except Exception` leaves that path out.
- **`readline()`, the decode and the split stay outside the `try`.** If the port itself dies that should be loud, not retried silently forever.
- **`continue` inside a `try` is fine** — it is not an exception, so the `try` does not intercept it; it goes straight to the top of the `while`. Relevant because the `KEY:` branch's unresolved-key guard uses one.
- **What it buys:** the blast radius from 4.33/4.36 is gone. An exception in *any* branch now costs one printed line instead of the entire companion app — Discord keybinds, `SHELL:` buttons and all.
- **Improvement worth making:** the message is currently `f'error: {e}'`, which says what broke but not which press caused it. `f"error on {line}: {type(e).__name__}: {e}"` adds the offending line (still in scope) and the exception class, which `{e}` alone drops.

**ESP32 boot noise reaching the `else` branch (diagnosed 21-8-2026, not a bug).** Launching the app prints half a dozen `cannot find the given category:` lines — `SPIWP`, `clk_drv`, `mode`, `load`. Opening the serial port toggles DTR/RTS, which **resets the board**, so the ROM bootloader's own startup chatter arrives as the first few lines. Those strings happen to contain colons, so they pass the `if ":" not in line` guard, split into a nonexistent category, and get correctly reported by the `else`. The `else` is doing its job. Options if it ever becomes annoying: leave it (it is also proof the board really did reset); or `time.sleep(2)` then `connection.reset_input_buffer()` before the loop, to discard the chatter rather than parse it. **Left alone for now.**

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

**The async structural question — RESOLVED 17-8-2026: `asyncio.run()` inside the handler.** As predicted. But the reasoning that settled it is worth keeping, because it is stronger than "the log said so":

**Making the read loop async would buy nothing today, because `connection.readline()` is blocking.** Wrapping the loop in `async def` and sprinkling `await`s does not change that — `readline()` is a synchronous call that sits there for up to a full second holding the event loop hostage. Async does not make blocking calls non-blocking; it only lets *other awaiting tasks* run while one task is genuinely suspended at an `await`. With one task and a blocking read, the restructure delivers all the ceremony and none of the concurrency.

Doing it properly would need `asyncio.to_thread()` around the read, or `pyserial-asyncio`, or a reader thread feeding a queue — a real restructure with a dependency question attached, not the ten-minute syntax change it looks like. **Worth knowing before roadmap item 6, where "make it async" will sound cheap and won't be.**

This still becomes a real decision at **roadmap item 6**: showing now-playing on the display means SMTC state arriving *while* the serial loop reads presses — two things genuinely in flight, which is the first point where async earns its keep rather than being a shape imposed by WinRT. Note the correction at the top of section 2 makes this *easier* than assumed: SMTC has change events, so this may be "react to a callback" rather than "poll on a timer."

### 8.7 SMTC capability surface and timeline mechanics (mapped 17-8-2026)

**`get_playback_info().controls` is the definitive answer to "what will Spotify actually honour"** — a set of `is_*_enabled` flags, enumerable with `dir()`. Printed once against desktop Spotify:

| Honoured | Not honoured |
|---|---|
| play_pause_toggle, next, previous, play, pause, stop, fast_forward, rewind, playback_position, shuffle, repeat | playback_rate, record, channel_up, channel_down |

**⚠️ These flags are live state, not a static capability table.** `is_play_enabled` read `False` purely because the track was playing at that moment; it would flip when paused. Only the genuinely-unsupported ones (rate, record, channel) read `False` regardless of state. **Do not cache this at startup and trust it.** Its real future use is roadmap item 6 — greying out a control the current app can't do, rather than showing a button that lies.

**Full SMTC surface, with what it's worth to this project:**

| Call | Spotify | Status here |
|---|---|---|
| `try_toggle_play_pause_async()` | ✅ | ✅ working on hardware |
| `try_skip_next_async()` / `try_skip_previous_async()` | ✅ | ✅ working on hardware |
| `try_change_playback_position_async(ticks)` | ✅ | ✅ written & tested (seek) |
| `try_change_shuffle_active_async(bool)` | ✅ | ✅ written & tested |
| `try_change_auto_repeat_mode_async(enum)` | ✅ | not written — three-state enum |
| `try_play_async()` / `try_pause_async()` / `try_stop_async()` | ✅ | not written — trivial |
| `try_fast_forward_async()` / `try_rewind_async()` | ✅ | **tested, then abandoned — see below** |
| `try_get_media_properties_async()` | ✅ | proven; `title` + `artist` the only useful fields (see 8.6) |
| `get_timeline_properties()` | ✅ | ✅ position read written & tested |
| Like / save track | ❌ | Web API only |
| Volume | ❌ | not a media-session concept — see 2.6 |

**Fast-forward / rewind: tested, work, and deliberately not used.** Both are honoured by Spotify and both jump a **fixed 5 seconds**. That briefly looked like free seek with no tick arithmetic — but there is no argument to pass, so a user-configurable offset is impossible. Rejected in favour of real seek, on the same "give the user the freedom" reasoning as 2.3. (They remain available as fixed-jump actions if ever wanted.)

**Timeline mechanics — the actual difficulty of seek, which was never the arithmetic:**

- **Read and write use different units.** `timeline.position` and `end_time` come back as Python **`timedelta`** objects (`.total_seconds()` → float), but `try_change_playback_position_async` takes an **int in ticks (100-nanosecond units)**. `int(seconds * 10_000_000)`. That asymmetry is the whole awkwardness; discovering it cost one `print(type(...))`.
- **The reported position is stale while playing, by 0.5 to 4 seconds.** Windows does not stream position — Spotify pushes a timeline update periodically and the OS hands back the last one it got. **Staleness and instantness are the same fact:** `get_timeline_properties()` needs no `await` precisely because it reads a cache rather than asking Spotify.
- **`last_updated_time` exists to fix this.** `position + (now_utc − last_updated_time)` gives the true position. Verified in practice; up to 4s of drift makes this worth doing even for a 10-second seek, where the error would otherwise be up to 40%.
- **The correction is only valid while PLAYING.** On pause, Spotify pushes one final accurate position, then `last_updated_time` freezes while the wall clock keeps moving — so `elapsed` starts measuring *how long you have been paused* and the correction adds time that never passed. Confirmed empirically by pausing for 30s and watching `corrected` run away from `reported`. Hence the `PlaybackStatus.PLAYING` branch (4.29).

**Seek edge cases — tested rather than guarded against.** A clamp (`max(0, min(target, end))`) was drafted defensively and then **deliberately not kept**, because testing showed Spotify already handles both ends: seeking before 0 lands at the track start, and seeking past the end **advances to the next track**. The latter is a design decision left as-is: it is Spotify's own behaviour, predictable, and preventing it would mean unexplained defensive code. Recorded here so a future reader knows the absence of a clamp is a decision, not an oversight.

> **⚠️ FLAG FOR ROADMAP ITEM 6 (raised deliberately 17-8-2026, do not lose):** any track position shown on the display is **approximate**, not exact — 0.5–4s of drift, reduced but not eliminated by the correction. **This must be communicated to the user in the interface** rather than presented as an exact readout. A progress bar that lags visibly looks broken in a way a seek button never does, which is also where the correction stops being optional and starts being required.

### 8.8 `spotify_functions.py` and parameterised actions (decided 17-8-2026 — **built 18/21-8-2026, see 8.9**)

**Rejected: one file per protocol category** (`key.py`, `shell.py`, `spotify.py`, `obs.py`). It mirrors the wire format rather than the code, and the shape doesn't survive contact: `shell.py` would be one line, `obs.py` would be empty (nothing written yet, shape unknown), while `spotify.py` is already the largest thing in the app. Two of four would be real, two ceremony — the same failure mode as forcing category dispatch into a dict (8.3).

**Decided: extract each branch body into a function inside `main.py` first** (`handle_key`, `handle_shell`, `handle_spotify`) — already next-steps item 11 from the previous session, and the prerequisite for *any* split, since a file split is just moving functions that already exist. Then `spotify_functions.py` splits out on its own merits: it brings its own dependency (`winrt`), its own async model, and session-finding logic nothing else touches. `shell.py` probably never earns it.

**Naming trap noted:** Python resolves local files before installed packages, so a file named `serial.py` would shadow pyserial with a confusing error. Not a problem with the names chosen, but `obs.py` sits close to a package that will be installed later.

**Scope decision — write functions for everything an action string can express, not just what's on the page.** Prompted by the student's point that a user's config layout may want shuffle-on, repeat-off, or stop even if none of those go on the author's own six buttons. The reachability gate is *"can an action string express it,"* not *"is it on my page."* This is a genuine extension of 2.3's user-freedom argument into the Python surface, where 2.3 had located that freedom only in `KEY:` and `SHELL:`.

**Consequence — the protocol now carries parameters, ahead of OBS.** `SPOTIFY:SEEKFWD:10` uses the same `split(":", 1)` idiom one level down, exactly as `OBS:SCENE:1` always implied. Decisions taken:
- **`SEEKFWD:n` / `SEEKBACK:n`, not `SEEK:±n`** — two named actions, always positive. Negative numbers are easy to mistype in a config file someone else is editing.
- **`SHUFFLE` / `SHUFFLE:ON` / `SHUFFLE:OFF`** — three action strings over two functions. `function_set_shuffle(session, bool)` talks to SMTC; `function_toggle_shuffle(session)` reads state and delegates to it. Explicit on/off is not redundant with toggle: a "start my session" macro that sets shuffle **on** is idempotent, where a toggle in the same macro is a coin flip. Same reason `try_play_async` exists alongside the toggle.
- **`playback_rate` is the one parameterised call not worth writing** — Spotify doesn't honour it (8.7).

**Session lifetime — settled deliberately.** The session is looked up **once per button press**, in the handler, and passed down as the first parameter to every function. Not cached at startup: a session object belongs to a *running instance* of Spotify, so a cached one dies on restart with failures that look like broken code rather than a restarted app. Not looked up per function either: the `None` guard would be duplicated a dozen times, and `function_try_seek` → `function_get_time_position` would do two lookups and could in principle get two different sessions.

**Dispatch: use a dict this time.** 8.3 said to revisit the dispatch-table question at the fourth category — this is it, and the answer differs *by level*. Top-level categories stay `if`/`elif` (four ever, non-uniform bodies). The Spotify second level is a dozen-plus actions mapping name → function with uniform signatures — data-to-data, exactly what `KEY_NAMES` is. Requires a uniform signature (`func(session, params=None)`) so the no-argument ones accept and ignore `params`; slightly ugly, and it keeps the table a table.

### 8.9 `spotify_functions.py` as actually built (18/21-8-2026)

8.8 planned this; this is what came out, including where the plan changed.

**File layout, top to bottom:** imports → SMTC-facing functions → wrappers → `SPOTIFY_FUNCTIONS` table → `function_handle_spotify_functions`. That order is forced, not stylistic (4.34).

**The invariant, and the only rule the file has:** *every value in the table is `async def f(session, params=None)`.* Reading down the dict's right-hand column and checking that one property is the entire correctness check for the dispatch layer.

**The wrapper layer — the main design decision of the session.** Protocol knowledge (parameters arrive as strings; `SEEKBACK` is a negative offset; `"ON"` means `True`) lives in the wrappers and the table. The SMTC-facing functions below them take **real Python types** — `function_try_seek(session, offset_seconds: float)`, `function_set_shuffle(session, bool)` — and know nothing about serial. Consequences: `spotify_functions.py` could be reused by something that isn't this protocol, and 4.37's silent-truthy-string bug becomes structurally impossible, because nothing typed is ever wired directly into the table.

Built this session: `function_shuffle_wrapper` (three action strings, one key), `function_seek_wrapper_forward` / `function_seek_wrapper_backward` (conversion + sign), and three transport one-liners `function_play_pause` / `function_skip_next_song` / `function_previous_song`. The `_wrapper` suffix is now a convention in this file.

**Lambdas were the plan and were dropped.** 8.8 and the early part of this session favoured `"SEEKBACK": lambda s, p: function_try_seek(s, -int(p))` — no extra names, conversion visible in the table. Rejected once 4.36 landed: **a lambda is one expression and cannot hold a `try`/`except`**, and the guard turned out to be mandatory rather than optional. Named wrappers can grow a guard; lambdas would have had to be rewritten into functions the moment one was needed. General form worth keeping: *choose the construct that can absorb the next requirement, when the next requirement is cheap to foresee.*

**`float`, not `int`, for seek offsets.** Strictly more permissive (`float("10")` → `10.0`, so every existing config keeps working), and consistent with 2.3's give-the-user-the-freedom line. Noted honestly: sub-second offsets sit inside the 0.5–4 s drift window (8.7), so the extra precision is nominal — it costs nothing, and pretending it's meaningful would be wrong.

**Uniform signature: the cost, paid deliberately.** About five functions carry a `params=None` they never read. This was pushed back on twice and the justification that finally held is mechanical: **there is one call site**, `await func(session, params)`, and it cannot know which function it pulled out of the table. A one-argument function handed two arguments is a `TypeError`. The middle option — dispatcher branches on whether params exist — collapses on `SHUFFLE`, because `SHUFFLE`, `SHUFFLE:ON` and `SHUFFLE:OFF` all reduce to the single key `"SHUFFLE"` after the split, so that one function needs `params` sometimes and not others regardless. The pattern is standard well beyond Python (a Django view takes `request` whether or not it reads it; `main(argc, argv)` takes both when you use neither): **a generic caller requires callees to agree on a shape, and the ignored parameter is what that agreement costs.**

**Why a table rather than the `if`/`elif` chain that would also have worked.** The elif version puts protocol knowledge inside control flow, so adding an action means editing a branch structure — where typos, missing `elif`s and fall-through live (cf. 4.26, 4.32, 4.33, all control-flow bugs). The dict is data: adding an action is adding a row, with none of those failure modes. It is also *inspectable* — you can assert that every action string the firmware sends has an entry, or print the table to see what the app supports. You cannot ask an elif chain what it handles. Same reasoning that made `KEY_NAMES` a dict in `main.py`. **Caveat recorded: this only holds because the actions genuinely are uniform** (name in, SMTC call out). If three of them needed materially different arguments, forcing a common signature would be the wrong move and elifs would be the honest shape.

**`GETTIME` deliberately left out of the table** (commented, not deleted). It is the only synchronous function in the file, and `await` on a float is a `TypeError`. It was **not** marked `async` to make it fit: in this project the `async` / `_async` marker is the one signal that has been consistently reliable, and `get_timeline_properties()` needing no `await` is the same fact as its position being stale (8.7). Marking an instant cache read as async to satisfy a table would spend that signal for nothing — and would force `asyncio.run()` around it in every test script and future display refresh. It also has no button and returns a value the dispatcher discards. **Re-add at roadmap item 6**, when the dispatcher's contract changes from fire-and-forget to caring about return values.

**Session lifetime, now actually enforced.** 8.8 settled that the session is looked up once per press in the handler and passed down. `function_show_now_playing` was violating it by finding its own session; it now takes one like everything else, with the `None` guard living only in the handler.

**Table as of 21-8-2026:** `SHUFFLE`, `SEEKFWD`, `SEEKBACK`, `SHOWDATA`, `PLAYPAUSE`, `NEXT`, `PREV`.

**Module-level test harness deleted (21-8-2026, second session)** — see 4.40. The file is now imports → functions → wrappers → table → handler, with **nothing that executes at import time**. That is the property `main.py` depends on, and it is now true rather than intended.

**✅ Closed 21-8-2026 (second session): the action strings are reconciled.** The reconciliation pass ran as part of the page rebuild, and the two sides now match exactly:

| Button | Firmware string | Table key | Params |
|---|---|---|---|
| Play/Pause | `SPOTIFY:PLAYPAUSE` | `PLAYPAUSE` | — |
| Previous | `SPOTIFY:PREV` | `PREV` | — |
| Next | `SPOTIFY:NEXT` | `NEXT` | — |
| Seek −10 | `SPOTIFY:SEEKBACK:10` | `SEEKBACK` | `"10"` |
| Seek +10 | `SPOTIFY:SEEKFWD:10` | `SEEKFWD` | `"10"` |
| Shuffle | `SPOTIFY:SHUFFLE` | `SHUFFLE` | — |

Dropped from the firmware because no table key exists and none is planned soon: `VOLUP`, `VOLDOWN`, `LIKE`, `REPEAT`, `MUTE`, `QUEUE`. Intermediate names `SHUFFLE_DECIDE` / `SHUFFLE_TOGGLE` were tried and dropped earlier. **The seek strings carry magnitude only** — the wrapper owns the sign (4.41). `SHOWDATA` remains in the table with no button.

### 8.10 `obs_functions.py` and the OBS integration (built 21-8-2026, second session)

**Library: `obsws-python`** (v5 protocol, built into OBS 28+ — no plugin). ⚠️ Not `obs-websocket-py`, which targets the dead v4 protocol; most tutorials online still use it. Same trap as the `winrt` package in 4.23.

**Connection: cached, built lazily.** Module-level `client = None`, and `function_get_client()` builds it on first use and returns the cached one afterwards, with `global client` so the assignment reaches module scope. Three options were weighed:

| Option | Verdict |
|---|---|
| Connect at import | Rejected — OBS closed at launch would kill the whole companion app before the read loop starts. 4.31/4.40 again. |
| **Lazy + cached** | **Chosen.** App starts fine with OBS closed; first press pays the connect. |
| Connect per press | Rejected, but *not* on speed — see below. |

**The responsiveness argument was raised and did not survive.** A local websocket connect is roughly 5–20 ms (TCP handshake plus the v5 SHA256 challenge-response); a scene switch is 1–2 ms. Per-press connecting would be ~10× the work but still far below the ~50 ms where a button feels laggy. **The real arguments for caching are different:** obs-websocket also *pushes events* (scene changed, stream started), and events only arrive on a connection that stays open — so per-press forecloses roadmap item 6. Plus one failure point instead of one per press. Recorded because the first argument offered (speed) was the weak one and the student picked the connection up on the strong one.

**The module is synchronous — deliberately unlike Spotify.** `ReqClient` has no coroutines, so there is no `async def`, no `await`, and `main.py` calls the handler directly with **no `asyncio.run()`**. Forcing it to match the Spotify module's uniform-async invariant would be cargo-culting; the two branches in `main.py` look different because the two libraries are different.

**Response objects unpack in two steps, and the naming is inconsistent by design of the library:** dot access for the response's own fields (`get_scene_list().scenes`, `get_current_program_scene().scene_name`), but **bracket access for nested contents** (`scene["sceneName"]`), because the library builds attributes for top-level fields and leaves the nested JSON as raw dicts. Verified live rather than assumed — `print(vars(response))` is the way to interrogate an unfamiliar response.

**Functions as built:**

| Table key | Function | Request | Params |
|---|---|---|---|
| `SCENE` | `function_set_scene` | `set_current_program_scene` | scene name |
| `STREAM` | `function_stream_toggle` | `toggle_stream` | — |
| `RECORD` | `function_record_toggle` | `toggle_record` | — |
| `RECORDPAUSE` | `function_record_pause_toggle` | `toggle_record_pause` | — |
| `MUTE` | `function_mute_toggle` | `toggle_input_mute` | input name |
| `HIDETOGGLE` | `function_visibility_toggle` | 4 calls, see below | source name |
| `CLIP` | `function_save_replay_buffer` | `save_replay_buffer` | — |

**OBS's data model, because it caused the most confusion this session:**

- **Source** — a thing producing picture or sound (webcam, window capture, mic). Exists once, globally, with a name you chose.
- **Scene** — a layout: which sources are visible, where.
- **Scene item** — *one source placed in one scene*. The same webcam in `Game` and in `Camera` is **two scene items backed by one source**, each with its own position, size and visible flag.

**Consequence, and the reason `function_visibility_toggle` takes four calls:** a source has no on/off switch. Visibility is a property of the *placement*, not the thing placed — deliberate, so that scenes stay independent layouts. "Turn the webcam off" is not well-formed until you say *off where*. So: current scene → item id for that source in that scene → read enabled → set the opposite. Read-modify-write, same shape as `function_toggle_shuffle`, because neither API offers a toggle.

**The contrast worth remembering: audio mute is global.** `toggle_input_mute` takes only a source name, no scene, because muting is a property of the input itself. Video visibility is per-placement, audio mute is per-source — same app, opposite models.

**Failure modes that are normal, not bugs:**

- `get_scene_item_id` **raises** (code 600, "no scene items were found...") when the source isn't in the current scene. It does not return `None`. Pressing Hide webcam on a scene without the webcam is therefore an exception, caught by `main.py`'s `try`/`except` and printed as a raw obs-websocket error.
- `RECORDPAUSE` errors when not recording; `CLIP` errors when the replay buffer isn't running.
- All three are the case for the two-way protocol eventually — the device could grey out what isn't currently valid instead of failing silently into a terminal nobody is watching.

**⚠️ Replay buffer settings — where they actually live (checked in-app, 21-8-2026):**

- **Buffer enable + maximum replay time:** Settings → Output → Replay Buffer. Present in **both** Simple and Advanced output modes (Advanced puts it on its own tab). Memory cost is shown live: 20 s ≈ 14 MB, 50 s ≈ 36 MB, held in RAM continuously while running.
- **Auto-start with streaming/recording:** **Settings → General**, in the Output section — *not* under Settings → Output, where it looks like it should be. Two wrong guesses were made before this was found by looking.
- The buffer must be **running** for `CLIP` to do anything; enabling it in settings is not the same as starting it. There is a Start Replay Buffer button in the main window's controls panel.

**⚠️ Also from the Simple-mode Output screen:** *"Recordings cannot be paused if the recording quality is set to Same as stream."* That setting is the current one, so **`RECORDPAUSE` will not work until Recording Quality is changed.** Unresolved at end of session.

**Machine-specific strings, 8.5 territory:** the five scene names, `Mic/Aux`, `Desktop Audio`, and `Webcam` all have to match what is configured in *this* OBS install, character for character. They live in the firmware's `ButtonDef` strings, consistent with how `SHELL:` paths are handled. The websocket password is currently hardcoded in `obs_functions.py` — **must move to a gitignored config or an environment variable before the repo goes public**, and note that a password committed once stays in git history after the line is deleted.

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

**Added 17-8-2026 (Spotify dispatcher + seek + shuffle session):**

- **The session opened by the student comparing two AI suggestions against each other.** A previous chat's proposed plan was pasted in next to this one's, and the student asked which was right. They differed on ordering: wire the Python handler first (because the firmware already sends `SPOTIFY:PLAYPAUSE`, so three buttons light up with no reflash) versus rebuild the pages first (fix the contract, then build against it). The second argument was weaker and was conceded — the action names were never genuinely uncertain, and `SHELL:` had already set the Python-first precedent. **Treating two AI outputs as competing claims to be adjudicated, rather than as instructions, is the natural extension of the log's existing habit of checking AI claims against reality.**
- **That adjudication immediately produced a caught error — in the *log itself*.** Verifying the disputed premise meant grepping `spotifyButtons[]` rather than trusting section 2.3, which revealed that 2.3's "transport moves to `KEY:MEDIA_*`" bullet had **never been applied** despite the section carrying a "migration complete" note. Fifth caught claim in the log's history, and the first found *in the log* rather than in an AI message. Worth stating for the reflection: a project log is a source that also needs checking, and its confident summary lines are exactly where drift hides.
- **Debugging by reading error messages properly, not by guessing.** Two `AttributeError`s were resolved by noticing that the message names the *class* it failed on (4.28) — telling the student they had the wrong object rather than the wrong attribute. Separately, when a `print()` of an enum was uninformative, `repr()` and `list(type(x))` were used to see the real member and the full value set (4.29), rather than trying comparisons until one worked.
- **A silent failure was caught by noticing a number that didn't change.** The `== "PLAYING"` bug produced no error at all; it surfaced because `corrected` printed identically to `reported` when it should have been ~4 seconds higher. This is the same instinct as the 4.15 flush-logging investigation — instrument the thing that *should* change, then check whether it did.
- **A defensive clamp was rejected on evidence.** The AI supplied `max(0, min(target, end))` for seek without knowing whether Spotify needed it; the student's response was "let's just see what happens" and tested all four edges. Spotify handles both itself, so the clamp was dropped. **This is the better instinct, and it was the student's:** unexplained defensive code is code nobody dares remove later. Same shape as the 2-8-2026 camera decision — cut on evidence, not on theory. (Both edge behaviours are now recorded in 8.7 so the *absence* of a clamp reads as a decision.)
- **A feature was rejected for the right reason after being proven to work.** `try_fast_forward_async` / `try_rewind_async` both work and jump a fixed 5 seconds, which would have made seek nearly free. The student chose real seek anyway, explicitly for user-configurable offsets — accepting more work to preserve a freedom no current button needs. Consistent with 2.3's reasoning applied to a new surface.
- **The scope of the Python API surface was argued for and won.** Told to write only functions reachable from the current button set, the student pushed back: a config-driven layout means the reachable set is whatever a *user* can type, not what the author put on a page. Correct, and it changed the plan (8.8). Third time in this log the student has thought about other people's use of the device rather than their own.
- **Async: asked "shall we make main.py async?" and got a *no* with reasons.** The answer — that an async read loop buys nothing while `readline()` blocks, and doing it properly needs `to_thread`/`pyserial-asyncio`/a reader thread — landed better than the previous session's abstract explanations because it was attached to a concrete decision the student had proposed. **The declared weak point from 3-8-2026 is measurably less weak:** the coroutine-never-awaited bug was hit twice (4.27) but recognised the second time, and by the end of the session the student was correctly distinguishing "needs `await`" from "returns immediately" by reading the `_async` suffix.
- **Questioned a suggestion instead of applying it:** asked why `function_get_time_position` shouldn't be `async` ("could it not have to wait to receive the timeline data?"). A fair challenge that produced the useful answer — the call is instant *because* it reads a cache, which is the same fact as the position being stale. Two things that had looked unrelated turned out to be one.
- **Asked for a line-by-line explanation and then asked again with real numbers** when the abstract version half-landed — the same "explain it from a different angle" move as 3-8-2026, now applied without hesitation. Also checked a hypothesis about the timezone offset ("is there a chance I'm just in UTC right now?") rather than accepting the explanation on authority; it was wrong, but checking was right.
- **Pushed back on verbosity**, twice: asked for a shorter form when an answer ran long, and asked directly what the `COUNT(arr)` problem actually *was* after it had been recommended four times without ever being explained. Both fair; the second in particular caught a real failure to justify a recommendation.
- **Asked “why can we not just split it further?” and it was the right question at the wrong target.** Splitting `SEEKFWD:10` was never the problem — the split hands back the *string* `"10"`, and the string is the problem. Worth recording because the instinct (reach for the mechanism already in the file before adding a new one) is a good one, and because the question exposed that three previous explanations had been answering something the student wasn't asking.
- **“wtf is a lambda anyways?” — and the first two explanations failed.** Both started from the dict use case and worked backwards. What landed was starting from `f = double` (a function is a value; you can put it in a variable) and only then showing it in a table. **The failure was the explanation's, not the student's**, and it is the same shape as the `COUNT(arr)` failure from the previous session: assuming the gap was in the syntax when it was in the concept underneath. Ending note: after all three attempts, the student chose **named wrappers over lambdas anyway** — which turned out to be the better call, because a lambda cannot hold the `try`/`except` that 4.36 made mandatory.
- **Pushed back on `params=None` twice before accepting it** (“just no params work aswell? not like it needs them”). Fair pushback, and the first answer — essentially “it's the convention” — deserved to bounce. It only became convincing when tied to the mechanism: one call site, no way to know which function came out of the table. **A design pattern asserted is not a design pattern justified.**
- **Asked explicitly for the reasoned professional answer** (“what would be good career standard... be a professor with years in code design”) and then took the longer answer over the shorter one. The trade — data over control flow, pay a small uniform cost to keep a boundary uniform — was accepted on the argument rather than on authority, and the caveat (elifs are honest when the callees genuinely differ) was retained.
- **The missing `await` on `function_find_spotify()` was flagged four times across two sessions.** It landed on the fourth, when the failure was finally spelled out concretely: the call builds a coroutine object, that object is not `None` so the guard passes, and the *next* line fails with an `AttributeError` on a coroutine. Restating “you're missing an `await`” three times taught nothing. **Fourth recurrence of the same meta-failure this project keeps producing — repeating a recommendation is not explaining it** — and the fix each time has been to describe the mechanism instead.
- **`return print(...)` was flagged twice and deliberately kept**, along with the one-line `if x: return y` style. Recorded as *a choice, not an oversight*: it's their file and their convention, and consistency within it beats an external style rule applied halfway.
- **Two ideas raised unprompted and parked with reasons rather than dropped:** the dual SMTC/Web-API backend (2.7) and album art on the display (2.8). Both were the student's own initiative, both got costed properly, and both came out with a named trigger for revisiting. The “give the user the option” instinct is a real extension of 2.3; the counter worth remembering is that **an option nobody can make an informed choice about should be a default, not a setting.**
- **Testing was done at the boundary before wiring, unprompted and thoroughly.** Every table entry was exercised from a scratch harness before `main.py` was touched — including the two paths that kill the read loop (`"DONG"` for the unknown action, `SEEKFWD` with no parameter), and including the case-mismatch and bad-parameter branches. This is the “test at the lowest layer first” habit applying itself without being asked.
- **⚠️ AI misfires this session, for the catalogue (which now stands at seven):** (1) a garbled explanation of the `params.upper()` normalisation point that had to be re-asked outright (“explain”); (2) a **false alarm about the student's own test output** — claiming that `shuffle:bla` returning an error implied something was secretly normalising case, when a case-mismatched key correctly failing the `not in` check produces exactly that result. Notable because it is **the first entry in the catalogue that was not caught** — it was ignored and moved past rather than challenged. Also (3) a “nit” about reordering the session lookup that was raised as if it mattered and then withdrawn on being questioned (“what does it matter”) — it didn't, much.
- **AI-authored code this session:** the drift-correction block and the seek/shuffle function bodies were given as sketches after the mechanics were explained, in the established boilerplate-handed-over pattern. Every one of them was typed out by the student with modifications, and the resulting bugs (4.27–4.29) were all in the student's integration of them — which is where the learning is. The `function_get_time_position` structure in particular went through four review rounds before it ran.

**Added 21-8-2026, second session (wiring, firmware rebuild, hardening):**

- **The session's opening question was answered by reading the files, not by asking.** The handoff notes flagged one unverified claim — whether `spotify_functions.py` was actually working from `main.py`. The code settled it: the import and the `SPOTIFY` branch were both there, but the handler was never awaited (4.39), so the wiring existed and could not have worked. Worth noting as method: *"has this been done?"* was a question about a file, and files can be read.
- **The log was wrong again, and again in the same direction.** 4.38 recorded the `{target:2f}` print as deleted. It hadn't been. That is now two confident past-tense claims in this document that didn't describe the code (cf. the 2.3 "migration complete" note, caught 17-8), and both were written by the AI. The generalisation earned on 17-8 holds: **a project log's summary lines are exactly where drift hides**, and the fix is to check the file rather than the log.
- **The student pushed back on the review's tone, and was right.** Direct quote: *"stop calling the amount of occurrences I made a mistake, it is very pety."* The reviews had been tagging findings with how many times a similar thing had happened before ("fifth occurrence of this pattern"). Counting recurrences is useful *in this log*, where the point is to see patterns over months; it is not useful attached to a live review, where it adds nothing to the fix and reads as scorekeeping. **Recorded because the correction improved the working relationship and cost nothing** — and because it is a real distinction between documentation and feedback that the AI had collapsed.
- **A flagged "bug" was overruled and kept as a feature.** `print(f"seeking to target: ...")` had been flagged in four consecutive sessions. The student's answer: *"I WANT the seeking to target to stay in there. I find it nice to have."* Correct on the merits — seek is the one button whose effect is invisible without looking at Spotify, so the print is the only feedback it has. Third style item now deliberately kept over a flag (`return print(...)`, the one-line `if`, this). The pattern is consistent enough to state as a rule: **flag it once, take the answer, stop.**
- **`COUNT(arr)` finally landed — on the fifth session, and only once it was explained.** The student asked outright: *"what is this COUNT(arr) that is constantly brought up?"* It had been recommended in four consecutive next-steps lists and **never once explained**, which is precisely why it never got built. The explanation that worked was the failure mode, not the syntax: too-low count hides buttons, too-high walks off the array and hands garbage to `lv_label_set_text` as a string pointer. **This is the same meta-failure the log has now catalogued four times** (`COUNT` itself, the missing `await`, lambdas, `params=None`): *repeating a recommendation is not explaining it*, and the fix each time has been to describe the mechanism. Notable that the break came from the student asking, not from the AI noticing.
- **"Give me the code, I don't get it" — and that was the right call.** A `sizeof`-division macro is boilerplate with no decision in it; there is nothing to discover by retyping it. Consistent with the LVGL-plumbing precedent from 26-7. The *understanding* that mattered (why a macro and not a function; why it can't work inside `buildPage` where the array has decayed to a pointer) was separable from the typing, and was covered.
- **A clarifying question prevented a real breakage.** Asked *"so I should put everything into one big `if __name__` statement?"* — which is exactly what a half-understood explanation would have produced, and it would have emptied the module: the `def`s and the dispatch table must run at import or `main.py` gets nothing. The question forced the useful distinction (**definitions run at import; execution goes under the guard**) that the original explanation had skipped.
- **The `try`/`except` was tested by deliberately breaking the thing it guards.** The student inserted a bogus name into `spotify_functions.py`, pressed Play/Pause, and watched the app print `NameError` three times and keep running. Testing the guard rather than trusting it, and the same instinct as the 17-8 "let's just see what happens" on the seek clamp. Also the first direct evidence for 4.33's blast-radius argument: before the guard, that one press would have taken Discord, the keybinds and the System page down with it.
- **First deletion in the firmware, and the ordering trap was dodged.** Collapsing Spotify from three pages to one meant removing two enum entries and two `pages[]` rows, which shifts every later page index. 8.2 has warned about this since July, but every previous edit had been an *addition*. Landed first try.
- **Three bugs in the page rebuild, all caught before flashing** (4.41). The one worth keeping is `SEEKBACK:-10`: the wrapper already negates, so the sign in the string cancelled it and the back button would have seeked forward. It is 8.9's wrapper rule seen from the other side — **if the wrapper owns a convention, the wire string must not encode it too.**
- **AI misfire this session, for the catalogue (now at eight).** Asked *"is this a problem?"* about the startup output, the answer described the symptom and the mechanism accurately but never said the obvious thing — *that's your scratch harness, it's disposable, delete it*. Two further turns then went into how to keep it running safely (`if __name__ == "__main__"`), which was solving a problem the student didn't have. The student's *"why didn't you say that"* was fair, and their follow-up — *"understood that is on me"* — was not: the question was well-formed and the answer was incomplete. **Recorded because the failure shape is specific and repeatable: naming a thing is not the same as saying what it is for.**
- **Priorities were set by the student, with reasons, against the log's own ordering.** The next-steps list had repeat and `pycaw` volume ahead of OBS. Both were deprioritised: repeat as low-value right now, and volume because `pycaw` moves the *mixer* rather than Spotify's own slider — *"might use API way for that, to have it done proper"* — which re-opens 2.6 rather than resolving it. **That is a real engineering judgement, not avoidance:** it identifies that the cheap option delivers a subtly different feature, and declines to build it on that basis. OBS chosen instead, and it is the last integration with no shortcut available.
- **AI-authored code this session:** the `COUNT` macro and the rewritten `pages[]` table (boilerplate, given on request). Everything else was the student's — the `asyncio.run()` wiring, the whole Spotify page rebuild, and the `try`/`except` block, which was submitted correct on the first pass with only the error-message detail (include the offending line and the exception class) suggested afterwards.

**Added 21-8-2026, second session, part two (the OBS build):**

- **The domain model was the hard part, not the code.** `obs_functions.py` reached seven working functions in about the time the Spotify module took to reach two, and none of the difficulty was Python. It was OBS's **source / scene / scene item** distinction — the student asked "why can we not just turn off source webcam?", which is the exact right question and has a real answer (visibility is a property of the placement, so scenes stay independent layouts). **Recorded because the pattern generalises:** the SMTC work's hard part was also conceptual (async, `last_updated_time` drift), not syntactic. The APIs are easy; their models are not.
- **A wrong argument was offered and correctly not bought.** The lazy-cached connection was first justified on *responsiveness*. Challenged directly — *"how much would it cost to do this on each button press? would it cost a lot of time responsiveness?"* — and the honest number (5–20 ms, imperceptible) **defeated the argument that had been given**. The decision stood on different grounds: obs-websocket pushes events, and events need a connection that stays open. The student's *"the protocol thing convinced me"* is the correct reason, and it is a better reason than the one first supplied. **This is the good version of the epistemics problem in section 2** — the claim was checkable, it got checked, and the conclusion survived on a repaired argument rather than the original one.
- **Two AI misfires in one exchange, both settled by looking rather than arguing.** Asked where the replay buffer's auto-start setting lives: first answer said Advanced output mode, second said Advanced's Replay Buffer tab. Both wrong, and the student produced a screenshot each time. It is actually under **Settings → General**. Cheap to be wrong about, but it is the same shape as 4.29 and 2.8 — **recalled UI layout and recalled API surfaces are the same kind of unreliable**, and the standing rule (send them to the source) should have applied to an app's own settings screen too, not just to Microsoft's docs.
- **The `params`-as-source-name rename came from a readability question, not a bug.** *"make the code more readable"* on `function_visibility_toggle` produced `source = params`, an alias line whose only job is that three following lines stop saying "param (e.g. webcam)". Related: the student's own comment draft said `get_scene_item_id` checks "if param is in it", which undersells it — it *fetches an id* and **raises** when absent. Precision in a comment matters most where the failure mode is non-obvious.
- **Naming got negotiated again, and the reasoning was about search-ability.** `function_visibility_toggle` was kept over any variant containing "source" (misleading — it toggles a *scene item*, which is the misconception the comment exists to prevent) or "object" (not an OBS term). Same class of decision as `SHUFFLE` over `SHUFFLE_TOGGLE`.
- **Two features were correctly declined after being scoped.** "Hide the webcam in *all* scenes" was costed — a loop, plus per-scene `try`/`except` because most scenes don't contain the source, plus the observation that *toggle* is meaningless across mixed states so it would have to be set-not-toggle — and the student's answer was *"just curious, I don't think it is necessary."* Auto-starting the replay buffer from `function_stream_toggle` was similarly declined once it became clear a blind second toggle assumes buffer state always matches stream state. **Both are the 17-8 "don't add defensive code without evidence" instinct applied to features.**
- **AI-authored code in the OBS build:** the `OBS_FUNCTIONS` dict (on request), the two-line `function_visibility_toggle` skeleton, the `obsScenes[]` / `obs2Buttons[]` arrays, and the corrected `function_get_client()` after four review passes on the student's versions. The student wrote the connection function's first three drafts, every handler, and every other table function.
- **The `global` keyword was the one genuinely new Python concept**, and it took two passes: first placed at module level (where it does nothing), then correctly inside the function. The failure it prevents is worth keeping — without it, `client = obs.ReqClient(...)` makes `client` local *for the whole function*, so the read on the line above raises `UnboundLocalError`. **The error lands on the read, not the write**, which makes it read as nonsense the first time.

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

### Async, and other Python worth noting (17-8-2026)

- **Calling an `async def` does not run it.** It builds a coroutine object, unstarted. Something must `await` it, or an event loop must drive it via `asyncio.run()`. This is the single most useful async fact so far — it explains 4.27 in both instances, and the `RuntimeWarning: coroutine was never awaited` message *is* the symptom.
- **`async` does not make blocking calls non-blocking.** It only lets other awaiting tasks run while one task is genuinely suspended *at an `await`*. A `while True` loop around a blocking `readline()` gains nothing from being made async — with one task and a blocking read, there is nothing to interleave with.
- **The `_async` suffix is a contract, not decoration.** WinRT marks anything that may take time; no suffix means "returns immediately, do not await." Practical test: if a call needs awaiting and doesn't get it, the next attribute access fails, because you're holding a coroutine rather than data.
- **`asyncio.sleep` vs `time.sleep`:** the first is a coroutine and needs `await` (and only works inside one); the second blocks the whole thread. `asyncio.wait` is neither — it takes a set of awaitables, not a duration.
- **`repr()` vs `print()`:** `print` shows the human-readable form, which for enums can be lossy enough to be useless. `repr()` shows the unambiguous one. `list(type(x))` on an enum member lists every member of its class. Reach for both when a print looks uninformative.
- **Naive vs aware datetimes** — see 4.30. Python refuses to subtract one from the other rather than guessing.
- **`timedelta` is a duration, not a point in time.** Two datetimes subtracted give one; `.total_seconds()` converts it to a float. `timeline.position` is already a `timedelta` (duration from track start), so it needs the conversion but not the subtraction.
- **f-string format specs:** `{x:.2f}` is fixed-point to two decimals; `{name:14}` pads to 14 characters (which is what lines values into a column). The colon starts the formatting instructions in both cases.
- **Dict comprehension** — `{k: f(k) for k in items if cond}`, the key/value sibling of a list comprehension. Used to turn the `dir()`-plus-`getattr()` capability scan into an actual queryable dict rather than a print loop.
- **`dir(obj)`** lists an object's attribute names, and `getattr(obj, name)` fetches one by name — the pair that makes it possible to enumerate an API surface you don't have docs for. Directly useful here, since the WinRT bindings have no Python documentation (8.6).
- **Local files shadow installed packages.** A file named `serial.py` in the project folder would be imported instead of pyserial. Worth remembering when naming modules after the thing they wrap.
- **Top-level code runs on import**, which is why a test harness at module level is fine in a script and a bug in a module (4.31).

---

## 12. Immediate Next Steps for Next Session

*(as of 21-8-2026, end of second session)*

**Done this session — five items closed, three of them long-outstanding:**

- **OBS integration built and working** (roadmap item 8, and the item the student chose over the log's own ordering). `obs_functions.py`, seven table entries, three firmware pages, scene switching verified from the touchscreen. See 8.10.

- **`main.py` awaits the handler** (4.39). `asyncio.run(...)` around the `SPOTIFY` branch; the module was imported but its coroutine was never driven, so every Spotify press had been doing nothing.
- **The Spotify page rebuild** (was item 3). Three pages → one, six buttons, action strings reconciled against `SPOTIFY_FUNCTIONS`. **All six verified on hardware:** Play/Pause, Previous, Next, Seek −10, Seek +10, Shuffle. Seek and shuffle are pressable from the device for the first time.
- **`COUNT(arr)`** (was item 2). Outstanding for five sessions. Every `pages[]` row and `pageCount` itself now compute their length.
- **`try`/`except` around the dispatch** (was item 4). `except Exception as e`, so a bad action string costs one printed line instead of the whole companion app.

Plus: the module-level test harness is gone from `spotify_functions.py` (4.40), so importing it no longer runs a seek attempt.

**Where the project actually stands:** the Spotify integration is *done for the built function set*. Everything the device can press works, and nothing on the page is decorative. Three Spotify things remain unbuilt and all three are **deliberately parked**, not blocked.

1. **Finish testing the OBS buttons that have never been pressed.** Scene switching is proven end-to-end; the rest are written but unexercised from the device. Two need OBS settings changed first:
   - ⚠️ **`RECORDPAUSE` cannot work** while Recording Quality is "Same as stream" — OBS says so on the Output settings screen. Change the quality, then test.
   - ⚠️ **`CLIP` needs the replay buffer running**, which is separate from enabling it. Auto-start is under **Settings → General**; the buffer itself and its duration are under Settings → Output → Replay Buffer.
   - `MUTE:Mic/Aux` and `MUTE:Desktop Audio` need those exact names to exist in the Audio Mixer panel.
   - `HIDETOGGLE:Webcam` currently targets a Color Source named `Webcam` added to the `Camera` scene for testing. Real webcam later; the function does not care which.
   - **Reconnect after an OBS restart is unhandled.** The cached client goes stale and every press then throws into `main.py`'s `try`/`except` forever, with no path back short of restarting the companion app. Not urgent, but it is the known hole in 8.10's design.
2. **Spotify volume — decide the mechanism before building** (2.6, now re-opened). `pycaw` moves Spotify's share of the Windows mixer; the Web API moves Spotify's own slider. The student wants the second. That makes volume, rather than Like, the feature that would justify the OAuth stack (2.7), so cost both paths together rather than separately. **Nothing to build until this is decided.**
3. **Repeat, and stop/play/pause** — deprioritised by the student, not dropped. When it comes back: repeat is three-state, needs its own enum import, and needs the `repr()` check from 4.29 before guessing the comparison — guessing has already cost two rounds once. It probably wants a wrapper like shuffle's, since `REPEAT` / `REPEAT:ONE` / `REPEAT:ALL` / `REPEAT:OFF` collapse to one key.
4. **Two small `main.py` improvements**, both a line each: include the offending `line` and `type(e).__name__` in the `except` message; and (optional) `time.sleep(2)` + `connection.reset_input_buffer()` before the loop to swallow the ESP32 boot chatter instead of parsing it into `cannot find the given category` lines.
5. **Decide whether Like earns the Web API** — unchanged, and now second in the queue behind volume for the same OAuth stack.
6. **Two-way protocol** (roadmap item 6) — the gate for a lot of parked work. SMTC has **change events**, so this may be "react to a callback" rather than "poll on a timer". This is also where the dispatcher's contract changes from fire-and-forget to caring about return values, which is when `GETTIME` and `SHOWDATA` become real table entries. ⚠️ Carry the drift flag from 8.7 into the interface work: any position shown is approximate (0.5–4 s) and must be communicated as such. ⚠️ Not the ten-minute change it looks like — `readline()` blocks, and `async` alone doesn't change that (`asyncio.to_thread()`, `pyserial-asyncio`, or a reader thread + queue).
7. **Async is still the declared weak point, but visibly less so.** This session's version of the coroutine bug was the *boundary* case (sync code calling an `async def`, 4.39) rather than a missing `await` inside async code — and `asyncio.run()` as the crossing is now understood as the answer, not a formula. That same call shape is what OBS will need if `obsws-python` turns out to be async. Keep taking it in small doses through items 1 and 6.
8. **Python tidy-up** (optional, unchanged): align naming to snake_case (currently mixed; the `function_` prefix and the `_wrapper` suffix are deliberate project conventions — do the realignment as its own commit, not halfway); auto-detect the COM port via the CP2102's USB VID/PID (`0x10C4`/`0xEA60`) instead of hardcoding COM13; wrap the port open in a retry so unplugging doesn't kill the script.
9. **Comment the machine-specific strings** (8.5) — still outstanding. Username path, PATH dependency and self-assigned Discord keybinds.
10. **Small leftovers in `spotify_functions.py`:** the comment above `function_try_seek` still names `function_seek_forward` / `function_seek_backward` (renamed since); `float(offset)` re-converts a value that is already a float; the duplicated error-message string in the two seek wrappers will drift if one is ever reworded; `{target:2f}` in the seek print wants its missing dot (the print itself **stays** — 4.38).
11. **Firmware leftovers:** the Spotify labels read `"Seek -10"` and `"Seek 10"` — inconsistent form. The `prevPage`/`nextPage` chain in `PageDef` is now **unused** by every page, since Spotify was the only chained category; keep it (the next multi-page category gets it free) but know that it is currently dead weight and untested by anything on the device.
12. **Parked with named triggers (all still live, none dropped):**
    - **Web API as a second backend** (2.7) — now most likely to be un-parked by **volume** rather than Like. Still parked, still liked.
    - **Album art on the display** (2.8) — strictly behind item 6; needs a binary transfer mode and a memory-budget review. ~2 s per cover at 115200 baud.
    - **`GETTIME` as a table entry** (8.9) — behind item 6.
    - **Config-driven layout / layout editor** (roadmap item 9) — the long-term destination that makes `SHELL:` and `KEY:` strings portable (8.5).