// StreamDeck firmware — multi-page LVGL button grid
// Display: ILI9488 3.5" 480x320 (landscape), SPI, resistive touch via XPT2046
// Board: classic ESP32 — manual BOOT/RST dance still required on every upload
//
// Gotchas for future readers:
// - LVGL does NOT track time by itself on Arduino: lv_tick_inc() in loop()
//   is mandatory. Without it, LVGL's internal timers never fire — the UI
//   draws once at boot and then input polling and refreshes are dead.
// - Full-screen framebuffer doesn't fit in RAM (480*320*2 ≈ 300KB), so LVGL
//   renders in 20-row strips into buf1 (partial render mode).

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

TFT_eSPI tft = TFT_eSPI();

static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 320;

lv_display_t *disp;

// Page & button definitions (the "data" the whole UI is built from)
// ============================================================

// One button on a page. Two kinds share this struct:
// - action button: 'action' is the exact string sent over serial on press
// - navigation button: 'action' is NULL, 'targetPage' is the page it opens
struct ButtonDef {
  const char *label;   // what's drawn on the button: icon symbol + "\n" + text
  const char *action;  // serial command, or NULL for navigation buttons
  int targetPage;      // only used when action == NULL
};

// One page/screen.
// parentPage -1 = no back button (home). prevPage/nextPage -1 = no arrow;
// a category with multiple pages links its sibling pages through these.
struct PageDef {
  const char *title;
  const ButtonDef *buttons;
  int buttonCount;
  int parentPage;
  int prevPage;
  int nextPage;
};

// Page indices — must match the order of the pages[] array below
enum {
  PAGE_HOME,
  PAGE_KEYS,
  PAGE_DISCORD,
  PAGE_SPOTIFY,   // Spotify 1/3
  PAGE_SPOTIFY2,  // Spotify 2/3
  PAGE_SPOTIFY3,  // spotify 3/3
  PAGE_OBS,
  PAGE_MEDIA,
  PAGE_SYSTEM
};

static const ButtonDef homeButtons[] = {
  { LV_SYMBOL_KEYBOARD "\nKeybinds", NULL, PAGE_KEYS },
  { LV_SYMBOL_CALL     "\nDiscord",  NULL, PAGE_DISCORD },
  { LV_SYMBOL_AUDIO    "\nSpotify",  NULL, PAGE_SPOTIFY },
  { LV_SYMBOL_VIDEO    "\nOBS",      NULL, PAGE_OBS },
  { LV_SYMBOL_PLAY     "\nMedia",    NULL, PAGE_MEDIA },
  { LV_SYMBOL_SETTINGS "\nSystem",   NULL, PAGE_SYSTEM },
};

static const ButtonDef keyButtons[] = {
  { LV_SYMBOL_COPY  "\nCopy",       "KEY:CTRL+C",      0 },
  { LV_SYMBOL_PASTE "\nPaste",      "KEY:CTRL+V",      0 },
  { LV_SYMBOL_CUT   "\nCut",        "KEY:CTRL+X",      0 },
  { LV_SYMBOL_LEFT  "\nUndo",       "KEY:CTRL+Z",      0 },
  { LV_SYMBOL_IMAGE "\nScreenshot", "KEY:WIN+SHIFT+S", 0 },
  { LV_SYMBOL_LIST  "\nSelect all", "KEY:CTRL+A",      0 },
};

static const ButtonDef discordButtons[] = {
  { LV_SYMBOL_MUTE       "\nMute",    "DISCORD:MUTE",    0 },
  { LV_SYMBOL_VOLUME_MID "\nDeafen",  "DISCORD:DEAFEN",  0 },
  { LV_SYMBOL_CALL       "\nPTT",     "DISCORD:PTT",     0 },
  { LV_SYMBOL_EYE_OPEN   "\nOverlay", "DISCORD:OVERLAY", 0 },
};

static const ButtonDef spotifyButtons[] = {
  { LV_SYMBOL_PLAY       "\nPlay/Pause", "SPOTIFY:PLAYPAUSE", 0 },
  { LV_SYMBOL_NEXT       "\nNext",       "SPOTIFY:NEXT",      0 },
  { LV_SYMBOL_PREV       "\nPrevious",   "SPOTIFY:PREV",      0 },
  { LV_SYMBOL_VOLUME_MAX "\nVol +",      "SPOTIFY:VOLUP",     0 },
  { LV_SYMBOL_VOLUME_MID "\nVol -",      "SPOTIFY:VOLDOWN",   0 },
  { LV_SYMBOL_OK         "\nLike",       "SPOTIFY:LIKE",      0 },
};

static const ButtonDef spotify2Buttons[] = {
  { LV_SYMBOL_SHUFFLE "\nShuffle",  "SPOTIFY:SHUFFLE",  0 },
  { LV_SYMBOL_LOOP    "\nRepeat",   "SPOTIFY:REPEAT",   0 },
  { LV_SYMBOL_MUTE    "\nMute",     "SPOTIFY:MUTE",     0 },
  { LV_SYMBOL_RIGHT   "\nSeek +10", "SPOTIFY:SEEKFWD",  0 },
  { LV_SYMBOL_LEFT    "\nSeek -10", "SPOTIFY:SEEKBACK", 0 },
  { LV_SYMBOL_LIST    "\nQueue",    "SPOTIFY:QUEUE",    0 },
};

static const ButtonDef spotify3Buttons[] = {
  { LV_SYMBOL_SHUFFLE "\nQueue",  "SPOTIFY:SHUFFLE",  0 },
  { LV_SYMBOL_SHUFFLE    "\nQueue",   "SPOTIFY:REPEAT",   0 },
  { LV_SYMBOL_SHUFFLE    "\nQueue",     "SPOTIFY:MUTE",     0 },
  { LV_SYMBOL_SHUFFLE   "\nQueue +10", "SPOTIFY:SEEKFWD",  0 },
  { LV_SYMBOL_SHUFFLE    "\nQueue -10", "SPOTIFY:SEEKBACK", 0 },
  { LV_SYMBOL_SHUFFLE    "\nQueue",    "SPOTIFY:QUEUE",    0 },
};

static const ButtonDef obsButtons[] = {
  { LV_SYMBOL_IMAGE  "\nScene 1", "OBS:SCENE:1", 0 },
  { LV_SYMBOL_IMAGE  "\nScene 2", "OBS:SCENE:2", 0 },
  { LV_SYMBOL_UPLOAD "\nStream",  "OBS:STREAM",  0 },
  { LV_SYMBOL_VIDEO  "\nRecord",  "OBS:RECORD",  0 },
  { LV_SYMBOL_MUTE   "\nMic",     "OBS:MIC",     0 },
  { LV_SYMBOL_AUDIO  "\nDesktop", "OBS:DESKTOP", 0 },
};

static const ButtonDef mediaButtons[] = {
  { LV_SYMBOL_PLAY       "\nPlay/Pause", "KEY:MEDIA_PLAYPAUSE", 0 },
  { LV_SYMBOL_NEXT       "\nNext",       "KEY:MEDIA_NEXT",      0 },
  { LV_SYMBOL_PREV       "\nPrevious",   "KEY:MEDIA_PREV",      0 },
  { LV_SYMBOL_VOLUME_MAX "\nVol +",      "KEY:MEDIA_VOLUP",     0 },
  { LV_SYMBOL_VOLUME_MID "\nVol -",      "KEY:MEDIA_VOLDOWN",   0 },
  { LV_SYMBOL_MUTE       "\nMute",       "KEY:MEDIA_MUTE",      0 },
};

static const ButtonDef systemButtons[] = {
  { LV_SYMBOL_CLOSE "\nLock PC", "KEY:WIN+L",  0 },
  { LV_SYMBOL_POWER "\nSleep",   "SYS:SLEEP", 0 },
};

// title, buttons, count, parent, prevPage, nextPage
static const PageDef pages[] = {
  { "Home",        homeButtons,     6, -1,        -1,           -1 },
  { "Keybinds",    keyButtons,      6, PAGE_HOME, -1,           -1 },
  { "Discord",     discordButtons,  4, PAGE_HOME, -1,           -1 },
  { "Spotify 1/3", spotifyButtons,  6, PAGE_HOME, -1,           PAGE_SPOTIFY2 },
  { "Spotify 2/3", spotify2Buttons, 6, PAGE_HOME, PAGE_SPOTIFY, PAGE_SPOTIFY3},
  {"Spotify 3/3", spotify3Buttons, 6, PAGE_HOME, PAGE_SPOTIFY2, -1},
  { "OBS",         obsButtons,      6, PAGE_HOME, -1,           -1 },
  { "Media",       mediaButtons,    6, PAGE_HOME, -1,           -1 },
  { "System",      systemButtons,   2, PAGE_HOME, -1,           -1 },
};

static const int pageCount = sizeof(pages) / sizeof(pages[0]);

// One LVGL screen object per page, filled in during setup()
static lv_obj_t *screens[pageCount];

// Colors (one place to retheme the whole device)
// ============================================================

static const uint32_t COL_BACKGROUND = 0x101418;
static const uint32_t COL_ACTION     = 0x2A2F3A;  // action buttons + top bar, idle
static const uint32_t COL_ACTION_PR  = 0x4A5568;  // action buttons + top bar, pressed
static const uint32_t COL_NAV        = 0xFF6B00;  // category buttons, idle
static const uint32_t COL_NAV_PR     = 0xFF9040;  // category buttons, pressed
static const uint32_t COL_TEXT       = 0xFFFFFF;

// Layout constants
// ============================================================

// 50px top bar: back button left, title center, prev/next arrows right.
// 3x2 grid below it: 3*140 wide + 4 gaps of 15 = 480; 2*115 + gaps in 270.
static const int gridCols = 3;
static const int btnW = 140, btnH = 115;
static const int xStart = 15, xStride = 155;
static const int yStart = 63, yStride = 128;

static const int barBtnW = 60, barBtnH = 40, barBtnY = 5;
static const int backX = 8;
static const int nextX = 412;  // right edge: 480 - 60 - 8
static const int prevX = 344;  // left of next, 8px gap

// LVGL <-> hardware glue
// ============================================================

// Partial line-buffer: LVGL renders the screen in 20-row strips into this,
// calling my_disp_flush once per strip
static uint16_t buf1[screenWidth * 20];

// Touch calibration, specific to this physical panel (TFT_eSPI cal sketch)
uint16_t calData[5] = { 230, 3539, 255, 3493, 5 };

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, true);
  tft.endWrite();

  lv_display_flush_ready(disp);
}

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

// Button event callbacks
// ============================================================

// Action button pressed: send its command line to the PC companion app.
// user_data is the ButtonDef this button was built from.
void actionButtonEvent(lv_event_t *e) {
  const ButtonDef *def = (const ButtonDef *)lv_event_get_user_data(e);
  Serial.println(def->action);
}

// Category button pressed: switch to its target page. Purely local, no serial.
void navButtonEvent(lv_event_t *e) {
  const ButtonDef *def = (const ButtonDef *)lv_event_get_user_data(e);
  lv_screen_load(screens[def->targetPage]);
}

// Top bar buttons all get the current PageDef as user_data; they differ only
// in which of its links they follow.
void backButtonEvent(lv_event_t *e) {
  const PageDef *page = (const PageDef *)lv_event_get_user_data(e);
  lv_screen_load(screens[page->parentPage]);
}

void prevPageEvent(lv_event_t *e) {
  const PageDef *page = (const PageDef *)lv_event_get_user_data(e);
  lv_screen_load(screens[page->prevPage]);
}

void nextPageEvent(lv_event_t *e) {
  const PageDef *page = (const PageDef *)lv_event_get_user_data(e);
  lv_screen_load(screens[page->nextPage]);
}

// Page builder — turns one PageDef into a full LVGL screen
// ============================================================

// Small helper for the three top-bar buttons (back / prev / next), which are
// identical except for position, symbol, and which callback they trigger
void makeBarButton(lv_obj_t *scr, int x, const char *symbol,
                   lv_event_cb_t callback, const PageDef *page) {
  lv_obj_t *btn = lv_button_create(scr);
  lv_obj_set_size(btn, barBtnW, barBtnH);
  lv_obj_set_pos(btn, x, barBtnY);
  lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACTION), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACTION_PR), LV_STATE_PRESSED);
  lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, (void *)page);

  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, symbol);
  lv_obj_center(label);
}

lv_obj_t *buildPage(const PageDef *page) {
  // NULL parent = create a top-level screen instead of a child widget
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BACKGROUND), 0);

  // Top bar: title always; back/prev/next only when the page links somewhere
  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, page->title);
  lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

  if (page->parentPage >= 0)
    makeBarButton(scr, backX, LV_SYMBOL_LEFT, backButtonEvent, page);
  if (page->prevPage >= 0)
    makeBarButton(scr, prevX, LV_SYMBOL_PREV, prevPageEvent, page);
  if (page->nextPage >= 0)
    makeBarButton(scr, nextX, LV_SYMBOL_NEXT, nextPageEvent, page);

  // The grid: same start + index * stride math as the manual version,
  // with row/column recovered from the flat index i
  for (int i = 0; i < page->buttonCount; i++) {
    const ButtonDef *def = &page->buttons[i];
    int col = i % gridCols;
    int row = i / gridCols;

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, btnW, btnH);
    lv_obj_set_pos(btn, xStart + col * xStride, yStart + row * yStride);

    // Press feedback: second bg color bound to LV_STATE_PRESSED — LVGL
    // swaps colors on touch-down/up by itself, no event code involved
    if (def->action != NULL) {
      lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACTION), 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACTION_PR), LV_STATE_PRESSED);
      lv_obj_add_event_cb(btn, actionButtonEvent, LV_EVENT_CLICKED, (void *)def);
    } else {
      lv_obj_set_style_bg_color(btn, lv_color_hex(COL_NAV), 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(COL_NAV_PR), LV_STATE_PRESSED);
      lv_obj_add_event_cb(btn, navButtonEvent, LV_EVENT_CLICKED, (void *)def);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, def->label);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
  }

  return scr;
}

// Setup & main loop
// ============================================================

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);
  tft.setTouch(calData);

  lv_init();

  disp = lv_display_create(screenWidth, screenHeight);
  lv_display_set_flush_cb(disp, my_disp_flush);
  lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_touch_read);

  // Build every page once at boot; switching pages later is just a
  // lv_screen_load() away, no rebuilding
  for (int i = 0; i < pageCount; i++) {
    screens[i] = buildPage(&pages[i]);
  }

  lv_screen_load(screens[PAGE_HOME]);
}

void loop() {
  lv_timer_handler();
  delay(5);
  lv_tick_inc(5);  // advance LVGL's clock by the 5ms we just slept — without
                   // this its timers see no elapsed time and never fire
}