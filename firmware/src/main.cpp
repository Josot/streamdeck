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
  tft.setRotation(1);              // 1 = landscape (480 wide, 320 tall)
  tft.fillScreen(TFT_BLACK);       // wipe whole screen to one color
  

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

//   // --- DRAWING SHAPES ---
//   // All coordinates are (x, y) from top-left corner. x = across, y = down.

//   tft.drawRect(10, 10, 100, 60, TFT_WHITE);        // outline rectangle: x, y, width, height, color
//   tft.fillRect(10, 80, 100, 60, TFT_BLUE);         // filled rectangle
//   tft.drawRoundRect(10, 150, 100, 60, 10, TFT_GREEN); // rounded outline: ...,corner radius, color
//   tft.fillRoundRect(120, 10, 100, 60, 10, TFT_RED);   // filled rounded

//   tft.drawCircle(170, 130, 30, TFT_YELLOW);        // outline circle: center x, center y, radius, color
//   tft.fillCircle(170, 200, 30, TFT_CYAN);          // filled circle

//   tft.drawLine(250, 10, 400, 100, TFT_MAGENTA);    // line: x1, y1, x2, y2, color

//   // --- TEXT ---
//   tft.setTextColor(TFT_WHITE, TFT_BLACK);  // text color, background color
//   tft.setTextSize(2);                      // size multiplier (1 = smallest)
//   tft.setCursor(250, 150);                 // where text starts (x, y)
//   tft.print("Hello!");                     // print text at cursor

//   tft.setTextSize(3);
//   tft.setCursor(250, 200);
//   tft.print(42);                           // can print numbers too
// }