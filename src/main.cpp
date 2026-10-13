#include <Arduino.h>
#include <M5Cardputer.h>

enum Screen : uint8_t { HOME, PIANO, TUNER, MUSIC, FILES, GALLERY, ABOUT };
Screen screen = HOME;
int menuIndex = 0;
int tunerIndex = 0;

const char* menuItems[] = {"PIANO", "TUNER", "MUSIC PLAYER", "SD EXPLORER", "GALLERY", "SYSTEM INFO"};
constexpr int MENU_COUNT = sizeof(menuItems) / sizeof(menuItems[0]);

void drawHeader(const char* title) {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(CYAN, BLACK);
  M5Cardputer.Display.setCursor(7, 5);
  M5Cardputer.Display.print("JOHNNY");
  M5Cardputer.Display.setTextColor(DARKGREY, BLACK);
  M5Cardputer.Display.setCursor(166, 5);
  M5Cardputer.Display.print("SYNTH OS");
  M5Cardputer.Display.drawFastHLine(7, 19, 226, 0x5A2D);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setCursor(8, 25);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.print(title);
  M5Cardputer.Display.setTextSize(1);
}

void drawHome() {
  drawHeader("APP LAUNCHER");
  for (int i = 0; i < MENU_COUNT; ++i) {
    int y = 48 + i * 12;
    bool selected = i == menuIndex;
    M5Cardputer.Display.fillRect(8, y - 1, 224, 11, selected ? 0x801F : BLACK);
    M5Cardputer.Display.setTextColor(selected ? WHITE : 0xAD7D, selected ? 0x801F : BLACK);
    M5Cardputer.Display.setCursor(14, y);
    M5Cardputer.Display.printf("%c  %s", selected ? '>' : ' ', menuItems[i]);
  }
  M5Cardputer.Display.setTextColor(DARKGREY, BLACK);
  M5Cardputer.Display.setCursor(8, 126);
  M5Cardputer.Display.print("W/S: MOVE   ENTER: OPEN");
}

void drawSplash() {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextSize(3);
  M5Cardputer.Display.setTextColor(CYAN, BLACK);
  M5Cardputer.Display.setCursor(37, 48);
  M5Cardputer.Display.print("JOHNNY");
  delay(180);
  for (int i = 0; i < 7; ++i) {
    int y = random(35, 100);
    M5Cardputer.Display.fillRect(20, y, random(35, 205), random(1, 5), i % 2 ? 0xF81F : CYAN);
    M5Cardputer.Display.setTextColor(i % 2 ? 0xF81F : WHITE, BLACK);
    M5Cardputer.Display.setCursor(37 + random(-5, 6), 48 + random(-3, 4));
    M5Cardputer.Display.print("JOHNNY");
    delay(55);
  }
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(CYAN, BLACK);
  M5Cardputer.Display.setCursor(37, 48);
  M5Cardputer.Display.print("JOHNNY");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(0xAD7D, BLACK);
  M5Cardputer.Display.setCursor(67, 83);
  M5Cardputer.Display.print("PERSONAL DECK / BOOT");
  delay(650);
}

void drawTuner() {
  drawHeader("TUNER");
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setCursor(12, 52);
  M5Cardputer.Display.print("SELECT INSTRUMENT");
  M5Cardputer.Display.fillRect(12, 68, 100, 29, tunerIndex == 0 ? 0x801F : 0x2104);
  M5Cardputer.Display.fillRect(124, 68, 100, 29, tunerIndex == 1 ? 0x801F : 0x2104);
  M5Cardputer.Display.setTextColor(WHITE, tunerIndex == 0 ? 0x801F : 0x2104);
  M5Cardputer.Display.setCursor(35, 79);
  M5Cardputer.Display.print("GUITAR");
  M5Cardputer.Display.setTextColor(WHITE, tunerIndex == 1 ? 0x801F : 0x2104);
  M5Cardputer.Display.setCursor(151, 79);
  M5Cardputer.Display.print("UKULELE");
  M5Cardputer.Display.setTextColor(YELLOW, BLACK);
  M5Cardputer.Display.setCursor(12, 108);
  M5Cardputer.Display.print("Audio input module: pending");
  M5Cardputer.Display.setTextColor(DARKGREY, BLACK);
  M5Cardputer.Display.setCursor(8, 126);
  M5Cardputer.Display.print("A/D: SELECT   ESC: BACK");
}

void drawPiano() {
  drawHeader("POCKET PIANO");
  const char* notes[] = {"C", "D", "E", "F", "G", "A", "B"};
  for (int i = 0; i < 7; ++i) {
    int x = 9 + i * 32;
    M5Cardputer.Display.fillRoundRect(x, 54, 29, 55, 2, WHITE);
    M5Cardputer.Display.setTextColor(BLACK, WHITE);
    M5Cardputer.Display.setCursor(x + 11, 91);
    M5Cardputer.Display.print(notes[i]);
  }
  M5Cardputer.Display.setTextColor(YELLOW, BLACK);
  M5Cardputer.Display.setCursor(8, 39);
  M5Cardputer.Display.print("KEY MAP / SOUND ENGINE PENDING");
  M5Cardputer.Display.setTextColor(DARKGREY, BLACK);
  M5Cardputer.Display.setCursor(8, 126);
  M5Cardputer.Display.print("ESC: BACK");
}

void drawInfo(const char* title, const char* a, const char* b) {
  drawHeader(title);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setCursor(10, 57);
  M5Cardputer.Display.println(a);
  M5Cardputer.Display.setCursor(10, 73);
  M5Cardputer.Display.println(b);
  M5Cardputer.Display.setTextColor(YELLOW, BLACK);
  M5Cardputer.Display.setCursor(10, 100);
  M5Cardputer.Display.print("MODULE UNDER DEVELOPMENT");
  M5Cardputer.Display.setTextColor(DARKGREY, BLACK);
  M5Cardputer.Display.setCursor(8, 126);
  M5Cardputer.Display.print("ESC: BACK");
}

void openSelected() {
  switch (menuIndex) {
    case 0: screen = PIANO; drawPiano(); break;
    case 1: screen = TUNER; drawTuner(); break;
    case 2: screen = MUSIC; drawInfo("MUSIC PLAYER", "SD audio library", "Playback engine pending"); break;
    case 3: screen = FILES; drawInfo("SD EXPLORER", "File browser / folders", "SD mount and preview pending"); break;
    case 4: screen = GALLERY; drawInfo("GALLERY", "Image thumbnails", "Image decoder pending"); break;
    default: screen = ABOUT; drawInfo("SYSTEM INFO", "JOHNNY OS / CARDPUTER-ADV", "ESP32-S3 / 8MB FLASH"); break;
  }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);
  randomSeed(esp_random());
  drawSplash();
  drawHome();
}

void loop() {
  M5Cardputer.update();
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
    delay(10);
    return;
  }
  auto keys = M5Cardputer.Keyboard.keysState();
  if (screen == HOME) {
    if (keys.tab) { menuIndex = (menuIndex + 1) % MENU_COUNT; drawHome(); }
    if (keys.enter) openSelected();
    if (keys.word.size()) {
      char c = keys.word[0];
      if (c == 'w' || c == 'W') { menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT; drawHome(); }
      if (c == 's' || c == 'S') { menuIndex = (menuIndex + 1) % MENU_COUNT; drawHome(); }
    }
  } else if (screen == TUNER && keys.word.size()) {
    char c = keys.word[0];
    if (c == 'a' || c == 'A' || c == 'd' || c == 'D') { tunerIndex = 1 - tunerIndex; drawTuner(); }
  }
  if (screen != HOME && keys.del) {
    screen = HOME;
    drawHome();
  }
  delay(20);
}
