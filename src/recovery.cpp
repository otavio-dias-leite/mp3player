#include <Arduino.h>
#include <M5Cardputer.h>
#include "johnny_version.h"

// Minimal recovery image: no Wi-Fi, SD, audio tasks or application services.
void setup() {
  Serial.begin(115200);
  delay(250);

  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);

  auto config = M5.config();
  M5Cardputer.begin(config, true);
  delay(150);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(180);
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(12, 22);
  M5Cardputer.Display.println("JOHNNY");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(12, 54);
  M5Cardputer.Display.setTextColor(TFT_GREEN, TFT_BLACK);
  M5Cardputer.Display.println("RECOVERY MODE");
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5Cardputer.Display.setCursor(12, 72);
  M5Cardputer.Display.printf("Version %s", JOHNNY_VERSION);
  M5Cardputer.Display.setCursor(12, 91);
  M5Cardputer.Display.println("Display + keyboard test");
  Serial.printf("JOHNNY RECOVERY %s: booted\n", JOHNNY_VERSION);
}

void loop() {
  M5Cardputer.update();
  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto keys = M5Cardputer.Keyboard.keysState();
    if (keys.word.length()) {
      Serial.printf("KEY: %s\n", keys.word.c_str());
      M5Cardputer.Display.fillRect(8, 108, 224, 20, TFT_BLACK);
      M5Cardputer.Display.setCursor(12, 112);
      M5Cardputer.Display.print(keys.word.substring(0, 24));
    }
  }
  delay(10);
}
