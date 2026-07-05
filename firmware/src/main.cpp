#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

TFT_eSPI tft = TFT_eSPI();

static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 320;

// boilerplate LVGL stuff
// -----------------------------------------------------------------------------------------------------

// A partial line-buffer, not a full-screen buffer — the whole screen's worth
// of pixels (480*320*2 bytes = ~300KB) won't fit in the ESP32's ~320KB usable
// RAM alongside everything else. LVGL redraws in horizontal strips instead.
static uint16_t buf1[screenWidth * 20];

// info from the touch calibration example from TFT_eSPI lib
uint16_t calData[5] = { 230, 3539, 255, 3493, 5 };

// Tells LVGL how to push its rendered pixels to the real screen
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)px_map, w * h, true);
  tft.endWrite();

  lv_display_flush_ready(disp);  // tells LVGL "done, send the next chunk"
}

// Tells LVGL how to check for touch, using the tft.getTouch() you already know
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
// -----------------------------------------------------------------------------------------------------

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

  // button
  lv_obj_t *btn = lv_button_create(lv_screen_active());
  lv_obj_set_size(btn, 120, 120);
  lv_obj_set_pos(btn, 40, 40);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF6B00), 0);

  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, "1");
}

void loop() {
  lv_timer_handler();  // lets LVGL do its rendering/input-processing work
  delay(5);
}