#include <Arduino.h>
#include <M5Cardputer.h>

enum Screen : uint8_t { HOME, TUNER, DRUMS, ABOUT };
Screen screen = HOME;
int menuIndex = 0;
const char* menuItems[] = {"Afinador", "Bateria", "Sobre"};
constexpr int MENU_COUNT = 3;

void drawHome() {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(CYAN, BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(8, 5);
  M5Cardputer.Display.println("JOHNNY");
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setCursor(8, 34);
  M5Cardputer.Display.println("CARDPUTER-ADV");
  for (int i = 0; i < MENU_COUNT; ++i) {
    M5Cardputer.Display.setCursor(16, 58 + i * 20);
    M5Cardputer.Display.setTextColor(i == menuIndex ? BLACK : WHITE,
                                     i == menuIndex ? CYAN : BLACK);
    M5Cardputer.Display.printf("%s %s", i == menuIndex ? ">" : " ", menuItems[i]);
  }
  M5Cardputer.Display.setTextColor(DARKGREY, BLACK);
  M5Cardputer.Display.setCursor(8, 122);
  M5Cardputer.Display.print("Setas: navegar  Enter: abrir");
}

void drawPanel(const char* title, const char* line1, const char* line2) {
  M5Cardputer.Display.fillScreen(BLACK);
  M5Cardputer.Display.setTextColor(CYAN, BLACK);
  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(8, 8);
  M5Cardputer.Display.println(title);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(WHITE, BLACK);
  M5Cardputer.Display.setCursor(8, 48);
  M5Cardputer.Display.println(line1);
  M5Cardputer.Display.setCursor(8, 64);
  M5Cardputer.Display.println(line2);
  M5Cardputer.Display.setTextColor(YELLOW, BLACK);
  M5Cardputer.Display.setCursor(8, 118);
  M5Cardputer.Display.print("Esc: voltar");
}

void openSelected() {
  if (menuIndex == 0) {
    screen = TUNER;
    drawPanel("AFINADOR", "Modulo de audio em", "implementacao/teste.");
  } else if (menuIndex == 1) {
    screen = DRUMS;
    drawPanel("BATERIA", "Pads e sons serao", "adicionados no modulo.");
  } else {
    screen = ABOUT;
    drawPanel("JOHNNY", "Firmware Cardputer-Adv", "Build experimental.");
  }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);
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
  } else if (keys.del) {
    screen = HOME;
    drawHome();
  }
  delay(20);
}
