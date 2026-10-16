/*
 * MP3 PLAYER - MP3 player para M5Stack Cardputer ADV
 * Versao 5.0
 *
 * v5.0: filtra pastas sem MP3 (busca recursiva em fatias), mostra capa JPEG
 * embutida em ID3 APIC no modo CAPA, corrige verificacao de caminho e colore
 * melhor o espectro. Capas são renderizadas uma vez para não pesar no loop.
 *
 * Dependencias: M5Cardputer (M5Unified/M5GFX) e ESP8266Audio (earlephilhower)
 * Codec nesta versao: MP3 (inclui VBR); capas ID3 APIC JPEG/PNG ate 24 KB.
 * Pasta do sketch: MP3_Player/MP3_Player.ino
 *
 * RECURSOS
 *  - Navegador de pastas: exibe apenas diretorios com MP3 (inclusive em
 *    subpastas), varrendo em fatias; fila = MP3 da pasta atual.
 *  - Musica continua tocando enquanto voce navega (ESC no player).
 *  - Troca de faixa sem pausa (gapless na pratica), proxima/anterior,
 *    aleatorio, repetir (off/todas/uma), avanca sozinho.
 *  - Busca +-10 s, duracao real (Xing/VBRI/CBR), barra de progresso.
 *  - Tags ID3v2/ID3v1 (titulo e artista), acentos convertidos para ASCII.
 *  - Equalizador de 5 bandas (+-12 dB), 6 presets, preamp automatico e
 *    limitador suave. Fade in/out em pausa e inicio (sem cliques).
 *  - Visualizador: barras ciano/azul/rosa, espelho, onda e capa ID3 JPEG/PNG.
 *  - Timer de sono, tela apaga sozinha, brilho, agitar para proxima (IMU
 *    BMI270 do ADV), bateria, ajustes salvos na memoria.
 *  - Protecao anti-travamento: decodificacao em fatias de 8 ms, leitura de
 *    pasta em fatias, deteccao de faixa travada/arquivo ruim e pulo
 *    automatico, SD com fallback de frequencia.
 *
 * TECLAS (H abre o tutorial em qualquer tela; H avanca as paginas)
 *  LISTA   ; . sobe/desce   , / pagina   ENTER abre/toca   DEL pasta acima
 *          TAB player   S ajustes   E equalizador   R atualiza
 *          ESPACO pausa  N P faixa  - = volume  1-6 preset  X para
 *  PLAYER  ESPACO pausa  N proxima  P anterior/reinicia  , / busca 10 s
 *          ; . volume    - = volume  1-6 preset  TAB equalizador
 *          S aleatorio   R repetir   V visual    T timer de sono
 *          X para        DEL/` volta a lista (musica continua)
 *  EQ      , / banda     ; . ganho   0 zera      TAB/DEL/` volta
 */

#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <math.h>
#include <new>

#include <AudioOutput.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>

#include "mp3_player_version.h"

namespace {

// ============================================================ hardware
constexpr int SD_CS   = 12;
constexpr int SD_SCK  = 40;
constexpr int SD_MISO = 39;
constexpr int SD_MOSI = 14;

// ============================================================ cores
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
uint16_t C_BG = rgb(8, 6, 10);
uint16_t C_RED = rgb(255, 40, 60);
uint16_t C_DARKRED = rgb(60, 8, 18);
uint16_t C_PANEL = rgb(40, 12, 20);
uint16_t C_PANEL2 = rgb(18, 8, 12);
uint16_t C_DIM = rgb(34, 22, 28);
constexpr uint16_t C_WHITE   = rgb(255, 255, 255);
constexpr uint16_t C_GREY    = rgb(150, 150, 160);
uint16_t C_CYAN = rgb(255, 40, 60);
constexpr uint16_t C_BLUE    = rgb(70, 120, 255);
constexpr uint16_t C_MAGENTA = rgb(255, 70, 190);
constexpr uint16_t C_ORANGE  = rgb(255, 140, 0);
constexpr uint16_t C_YELLOW  = rgb(255, 230, 60);

// ============================================================ constantes
constexpr size_t SD_AUDIO_BUFFER_SIZE = 32 * 1024;
constexpr size_t BLOCK_SAMPLES = 1024;  // amostras por bloco enviado ao speaker
constexpr size_t RING_BLOCKS   = 4;     // speaker segura no maximo 2 por vez
constexpr size_t MAX_ENTRIES   = 200;
constexpr size_t NAME_SIZE     = 80;
constexpr size_t DIR_SIZE      = 112;
constexpr size_t PATH_SIZE     = 200;
constexpr int    EQ_BANDS      = 5;
constexpr int    SPEC_BANDS    = 12;
constexpr int    EQ_LIMIT      = 12;
constexpr int    NUM_PRESETS   = 6;
constexpr int    ROWS          = 4;
constexpr float  SHAKE_G       = 2.6f;

static uint8_t sdAudioBuffer[SD_AUDIO_BUFFER_SIZE];

// ============================================================ estado geral
enum Screen { SCR_BROWSER, SCR_PLAYER, SCR_EQ, SCR_SETTINGS, SCR_HELP };
Screen screen = SCR_BROWSER;
Screen returnScreen = SCR_BROWSER;
int helpPage = 0;

enum FadeAction { FA_NONE, FA_PAUSE, FA_STOP };
FadeAction fadeAction = FA_NONE;

bool sdReady = false;
bool playing = false;   // decoder ativo (inclui pausado)
bool paused = false;
bool imuOk = false;

int volumePercent = 80;
int volByMode[2] = {80, 30};  // 0 alto-falante, 1 fone
int outMode = 0;
int eqGain[EQ_BANDS] = {3, 1, 0, 2, 3};
int eqSelected = 0;
int settingsSel = 0;
int sleepIdx = 0;
uint32_t sleepDeadline = 0;

struct Options {
  int brightness = 70;
  int screenOffIdx = 0;
  int shake = 0;
  int repeat = 0;   // 0 off, 1 todas, 2 uma
  int shuffle = 0;
  int visual = 0;   // 0 barras, 1 espelho, 2 onda, 3 capa
} opt;

const int SCREEN_OFF_SEC[4] = {0, 15, 30, 60};
const int SLEEP_MIN[5] = {0, 15, 30, 60, 90};
const char *REPEAT_NAMES[3] = {"DESLIGADO", "TODAS", "UMA"};
const char *VISUAL_NAMES[4] = {"BARRAS", "ESPELHO", "ONDA", "CAPA"};

const char *EQ_LABELS[EQ_BANDS] = {"60", "250", "1K", "4K", "12K"};
const float EQ_FREQS[EQ_BANDS] = {60.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f};
const char *PRESET_NAMES[NUM_PRESETS] = {"BALANCE", "FLAT", "BASS", "ROCK", "VOCAL", "ELETRO"};
const int8_t PRESETS[NUM_PRESETS][EQ_BANDS] = {
  { 3,  1,  0,  2,  3},
  { 0,  0,  0,  0,  0},
  { 7,  4,  0,  0,  1},
  { 4,  1, -2,  3,  4},
  {-3, -1,  3,  3,  1},
  { 6,  3, -1,  2,  5},
};

char toastText[40] = {0};
uint32_t toastUntil = 0;
bool toastActive = false;

int batteryPercent = -1;
uint32_t lastBatteryRead = 0;

bool settingsDirty = false;
uint32_t settingsChangedAt = 0;
Preferences prefs;

struct ThemePalette {
  const char *name;
  uint8_t r, g, b;
  uint8_t darkR, darkG, darkB;
  uint8_t panelR, panelG, panelB;
  uint8_t panel2R, panel2G, panel2B;
  uint8_t dimR, dimG, dimB;
};
const ThemePalette THEMES[] = {
  {"DARK RED",   255, 45, 65,  60, 8, 18,  40, 12, 20,  18, 8, 12,  34, 22, 28},
  {"MATRIX",      55, 255, 105,  4, 48, 18,  8, 34, 16,  3, 16, 8,  15, 48, 25},
  {"CYBERPUNK",  205, 75, 255,  48, 12, 72,  38, 13, 58,  18, 7, 28,  48, 28, 62},
  {"ICE BLUE",     0, 215, 255,  5, 32, 62,  8, 28, 48,  5, 15, 28,  20, 45, 65},
  {"AMBER",      255, 165, 45,  62, 32, 5,  48, 27, 8,  20, 13, 5,  52, 38, 20}
};
constexpr int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);
int themeIndex = 0;

void applyTheme() {
  const ThemePalette &t = THEMES[constrain(themeIndex, 0, THEME_COUNT - 1)];
  C_RED = C_CYAN = rgb(t.r, t.g, t.b);
  C_DARKRED = rgb(t.darkR, t.darkG, t.darkB);
  C_PANEL = rgb(t.panelR, t.panelG, t.panelB);
  C_PANEL2 = rgb(t.panel2R, t.panel2G, t.panel2B);
  C_DIM = rgb(t.dimR, t.dimG, t.dimB);
}

bool screenOff = false;
uint32_t lastActivity = 0;

M5Canvas ui(&M5Cardputer.Display);
M5Canvas live(&M5Cardputer.Display);
M5Canvas coverCanvas(&M5Cardputer.Display);
SPIClass sdSPI(FSPI);

void drawBrowser();
void drawPlayer();
void drawEq();
void drawSettings();
void drawHelp();
void redrawCurrent();

// ============================================================ texto
const char LATIN_MAP[] =
    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

void safeCopy(char *dst, size_t n, const char *src) {
  if (!dst || !n) return;
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

void appendCp(char *out, size_t &n, size_t cap, uint32_t cp) {
  char c;
  if (cp >= 0x20 && cp < 0x7F) c = (char)cp;
  else if (cp >= 0xC0 && cp <= 0xFF) c = LATIN_MAP[cp - 0xC0];
  else if (cp == 0xA0) c = ' ';
  else if (cp < 0x20 || cp == 0x7F) return;
  else if (cp == 0x2018 || cp == 0x2019) c = '\'';
  else if (cp == 0x201C || cp == 0x201D) c = '"';
  else if (cp == 0x2013 || cp == 0x2014) c = '-';
  else c = '?';
  if (n + 1 < cap) out[n++] = c;
}

// enc: 0 Latin-1 (aceita UTF-8 valido), 1 UTF-16 com BOM, 2 UTF-16BE, 3 UTF-8
void decodeText(const uint8_t *p, size_t len, int enc, char *out, size_t cap) {
  size_t n = 0;
  if (enc == 1 || enc == 2) {
    bool be = (enc == 2);
    size_t i = 0;
    if (enc == 1 && len >= 2) {
      if (p[0] == 0xFF && p[1] == 0xFE) { be = false; i = 2; }
      else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; i = 2; }
    }
    for (; i + 1 < len; i += 2) {
      uint32_t cp = be ? (((uint32_t)p[i] << 8) | p[i + 1])
                       : (((uint32_t)p[i + 1] << 8) | p[i]);
      if (cp == 0) break;
      if (cp >= 0xD800 && cp <= 0xDBFF) { i += 2; cp = 0xFFFD; }
      else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
      appendCp(out, n, cap, cp);
    }
  } else {
    for (size_t i = 0; i < len;) {
      const uint8_t b = p[i];
      if (b == 0) break;
      if (b < 0x80) { appendCp(out, n, cap, b); ++i; }
      else if (b >= 0xC2 && b <= 0xDF && i + 1 < len && (p[i + 1] & 0xC0) == 0x80) {
        appendCp(out, n, cap, ((uint32_t)(b & 0x1F) << 6) | (p[i + 1] & 0x3F));
        i += 2;
      } else if (b >= 0xE0 && b <= 0xEF && i + 2 < len &&
                 (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80) {
        appendCp(out, n, cap, ((uint32_t)(b & 0x0F) << 12) |
                              ((uint32_t)(p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F));
        i += 3;
      } else if (b >= 0xF0 && b <= 0xF4 && i + 3 < len &&
                 (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80 &&
                 (p[i + 3] & 0xC0) == 0x80) {
        appendCp(out, n, cap, 0xFFFD);
        i += 4;
      } else { appendCp(out, n, cap, b); ++i; }  // Latin-1
    }
  }
  if (cap) out[n < cap ? n : cap - 1] = '\0';
  while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
}

void toAscii(const char *src, char *out, size_t cap) {
  decodeText((const uint8_t *)src, strlen(src), 3, out, cap);
}

bool hasMp3Extension(const char *name) {
  const size_t n = name ? strlen(name) : 0;
  return n >= 4 && name[n - 4] == '.' && tolower(name[n - 3]) == 'm' &&
         tolower(name[n - 2]) == 'p' && name[n - 1] == '3';
}

void fitText(LovyanGFX &g, const char *src, int maxW, char *out, size_t n) {
  safeCopy(out, n, src);
  if (g.textWidth(out) <= maxW) return;
  size_t len = strlen(out);
  const int dots = g.textWidth("..");
  while (len > 0) {
    out[len] = '\0';
    if (g.textWidth(out) + dots <= maxW) break;
    --len;
  }
  strncat(out, "..", n - strlen(out) - 1);
}

bool buildPath(char *out, size_t n, const char *dir, const char *name) {
  const int r = (dir[0] == '/' && dir[1] == 0) ? snprintf(out, n, "/%s", name)
                                               : snprintf(out, n, "%s/%s", dir, name);
  return r > 0 && (size_t)r < n;
}

void showToast(const char *msg) {
  safeCopy(toastText, sizeof(toastText), msg);
  toastUntil = millis() + 1800;
  toastActive = true;
}

// ============================================================ DSP
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float z1 = 0, z2 = 0;
  bool active = false;

  // type: 0 peaking, 1 low shelf, 2 high shelf (RBJ)
  void configure(int type, float fs, float f, float gainDb, float q) {
    if (f > fs * 0.45f) f = fs * 0.45f;
    const float A = powf(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * PI * f / fs;
    const float cw = cosf(w0);
    const float sw = sinf(w0);
    const float alpha = sw / (2.0f * q);
    float nb0, nb1, nb2, na0, na1, na2;
    if (type == 0) {
      nb0 = 1 + alpha * A; nb1 = -2 * cw; nb2 = 1 - alpha * A;
      na0 = 1 + alpha / A; na1 = -2 * cw; na2 = 1 - alpha / A;
    } else {
      const float t = 2.0f * sqrtf(A) * alpha;
      if (type == 1) {
        nb0 = A * ((A + 1) - (A - 1) * cw + t);
        nb1 = 2 * A * ((A - 1) - (A + 1) * cw);
        nb2 = A * ((A + 1) - (A - 1) * cw - t);
        na0 = (A + 1) + (A - 1) * cw + t;
        na1 = -2 * ((A - 1) + (A + 1) * cw);
        na2 = (A + 1) + (A - 1) * cw - t;
      } else {
        nb0 = A * ((A + 1) + (A - 1) * cw + t);
        nb1 = -2 * A * ((A - 1) + (A + 1) * cw);
        nb2 = A * ((A + 1) + (A - 1) * cw - t);
        na0 = (A + 1) - (A - 1) * cw + t;
        na1 = 2 * ((A - 1) - (A + 1) * cw);
        na2 = (A + 1) - (A - 1) * cw - t;
      }
    }
    b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0;
    a1 = na1 / na0; a2 = na2 / na0;
    active = fabsf(gainDb) > 0.05f;
  }

  inline float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void reset() { z1 = z2 = 0; }
};

Biquad eq[EQ_BANDS];

float gRate = 44100.0f;
float gPreamp = 1.0f;
float gVolTarget = 0.5f;
float gVolCur = 0.5f;
float gFadeGain = 0.0f;
float gFadeTarget = 1.0f;
float gFadeStep = 0.001f;
uint32_t gSamplesSubmitted = 0;

float gBand[SPEC_BANDS] = {0};
int16_t gScope[128] = {0};
const float SPEC_FREQS[SPEC_BANDS] = {60, 100, 170, 300, 500, 800, 1300, 2000, 3200, 5000, 8000, 12000};
float gGoertzel[SPEC_BANDS];
bool gGoertzelOk[SPEC_BANDS];

// Anel de buffers global: o speaker guarda PONTEIROS, entao os blocos precisam
// sobreviver a troca de faixa (gapless) e nunca ser reescritos enquanto tocam.
int16_t gRing[RING_BLOCKS][BLOCK_SAMPLES];
size_t gCur = 0;
size_t gFill = 0;
int gSliceLeft = 0;  // amostras que o decoder pode entregar nesta chamada de loop()

void configureEqualizer() {
  int maxBoost = 0;
  for (int i = 0; i < EQ_BANDS; ++i) {
    const int type = (i == 0) ? 1 : (i == EQ_BANDS - 1 ? 2 : 0);
    const float q = (type == 0) ? 1.0f : 0.707f;
    eq[i].configure(type, gRate, EQ_FREQS[i], (float)eqGain[i], q);
    if (eqGain[i] > maxBoost) maxBoost = eqGain[i];
  }
  gPreamp = powf(10.0f, (-0.55f * (float)maxBoost) / 20.0f);
}

void resetEq() { for (int i = 0; i < EQ_BANDS; ++i) eq[i].reset(); }

void configureSpectrum() {
  for (int i = 0; i < SPEC_BANDS; ++i) {
    if (SPEC_FREQS[i] < gRate * 0.45f) {
      gGoertzel[i] = 2.0f * cosf(2.0f * PI * SPEC_FREQS[i] / gRate);
      gGoertzelOk[i] = true;
    } else {
      gGoertzel[i] = 0;
      gGoertzelOk[i] = false;
    }
  }
}

void updateVolumeTarget() {
  const float v = (float)volumePercent / 100.0f;
  gVolTarget = v * v * (outMode == 0 ? 1.6f : 1.0f);  // reforco no alto-falante (o limitador protege)
}

void analyzeBlock(const int16_t *b, size_t n) {
  if (!n) return;
  for (int i = 0; i < SPEC_BANDS; ++i) {
    if (!gGoertzelOk[i]) { gBand[i] = 0; continue; }
    const float c = gGoertzel[i];
    float s1 = 0, s2 = 0;
    for (size_t k = 0; k < n; ++k) {
      const float s0 = (float)b[k] + c * s1 - s2;
      s2 = s1;
      s1 = s0;
    }
    float p = s1 * s1 + s2 * s2 - c * s1 * s2;
    if (p < 0) p = 0;
    const float amp = sqrtf(p) * 2.0f / (float)n;
    const float db = 20.0f * log10f(amp / 32768.0f + 1e-6f) + (float)i * 0.9f;
    float lvl = (db + 54.0f) / 54.0f;
    if (lvl < 0) lvl = 0;
    if (lvl > 1) lvl = 1;
    gBand[i] = lvl;
  }
}

bool submitBlock() {
  if (!gFill) return true;
  // playRaw assincrono: nunca aguarda o fim do audio no loop principal.
  // Se a fila interna estiver cheia, mantemos o bloco pendente e tentamos
  // novamente na proxima iteracao, sem sobrescrever memoria em uso.
  if (!M5Cardputer.Speaker.playRaw(gRing[gCur], gFill, (uint32_t)gRate,
                                   false, 1, 0, true)) {
    return false;  // fila cheia = contrapressao normal
  }
  analyzeBlock(gRing[gCur], gFill);
  if (gFill >= BLOCK_SAMPLES) {
    for (int k = 0; k < 128; ++k) gScope[k] = gRing[gCur][k * 8];
  }
  gSamplesSubmitted += (uint32_t)gFill;
  gCur = (gCur + 1) % RING_BLOCKS;
  gFill = 0;
  return true;
}

class Mp3AudioOutput final : public AudioOutput {
public:
  bool begin() override {
    gFill = 0;
    applyRate();
    return true;
  }

  bool SetRate(int hz) override {
    AudioOutput::SetRate(hz);
    applyRate();
    return true;
  }

  bool isFull() const { return gFill >= BLOCK_SAMPLES; }

  bool ConsumeSample(int16_t sample[2]) override {
    // Se o bloco anterior ainda esta pendente e nao da para enviar, NAO
    // consome a amostra: o decoder repete depois. Nunca passa do array.
    if (gSliceLeft <= 0) return false;  // fatia esgotada: devolve o controle ao loop()
    if (gFill >= BLOCK_SAMPLES && !submitBlock()) return false;
    --gSliceLeft;

    float x = ((float)sample[0] + (float)sample[1]) * 0.5f;  // speaker mono
    for (int i = 0; i < EQ_BANDS; ++i) {
      if (eq[i].active) x = eq[i].process(x);
    }
    gVolCur += (gVolTarget - gVolCur) * 0.0015f;
    if (gFadeGain < gFadeTarget) {
      gFadeGain += gFadeStep;
      if (gFadeGain > gFadeTarget) gFadeGain = gFadeTarget;
    } else if (gFadeGain > gFadeTarget) {
      gFadeGain -= gFadeStep;
      if (gFadeGain < gFadeTarget) gFadeGain = gFadeTarget;
    }
    x *= gPreamp * gVolCur * gFadeGain;

    // Limitador suave acima de 22000 (sem estalo de clipping)
    const float a = fabsf(x);
    if (a > 22000.0f) {
      const float over = (a - 22000.0f) / 10767.0f;
      x = copysignf(22000.0f + 10767.0f * tanhf(over), x);
    }

    gRing[gCur][gFill++] = (int16_t)x;
    if (gFill >= BLOCK_SAMPLES) submitBlock();  // se falhar, fica pendente
    return true;
  }

  void flush() override {
    const uint32_t t = millis();
    while (gFill && !submitBlock() && millis() - t < 150) delay(2);
  }

  bool stop() override {
    flush();  // nunca corta o que ainda esta tocando
    return true;
  }

private:
  void applyRate() {
    gRate = (hertz >= 8000 && hertz <= 96000) ? (float)hertz : 44100.0f;
    gFadeStep = 1.0f / (0.03f * gRate);
    configureEqualizer();
    configureSpectrum();
  }
};

AudioFileSourceSD *source = nullptr;
AudioFileSourceBuffer *bufferedSource = nullptr;
AudioGeneratorMP3 *decoder = nullptr;
Mp3AudioOutput *audioOutput = nullptr;

// ============================================================ configuracoes
void applyBrightness() {
  M5Cardputer.Display.setBrightness((uint8_t)(opt.brightness * 255 / 100));
}

void markSettingsDirty() {
  settingsDirty = true;
  settingsChangedAt = millis();
}

void loadSettings() {
  prefs.begin("mp3player", true);
  volByMode[0] = constrain(prefs.getInt("vs", 80), 0, 100);
  volByMode[1] = constrain(prefs.getInt("vp", 30), 0, 100);
  outMode = constrain(prefs.getInt("om", 0), 0, 1);
  volumePercent = volByMode[outMode];
  opt.brightness = constrain(prefs.getInt("br", 70), 10, 100);
  opt.screenOffIdx = constrain(prefs.getInt("so", 0), 0, 3);
  opt.shake = constrain(prefs.getInt("sh", 0), 0, 1);
  opt.repeat = constrain(prefs.getInt("rp", 0), 0, 2);
  opt.shuffle = constrain(prefs.getInt("sf", 0), 0, 1);
  opt.visual = constrain(prefs.getInt("vz", 0), 0, 3);
  themeIndex = constrain(prefs.getInt("th", 0), 0, THEME_COUNT - 1);
  for (int i = i < EQ_BANDS; ++i) {
    char key[3] = {'g', (char)('0' + i), 0};
    eqGain[i] = constrain(prefs.getInt(key, eqGain[i]), -EQ_LIMIT, EQ_LIMIT);
  }
  prefs.end();
  applyTheme();
}

void saveSettings() {
  prefs.begin("mp3player", false);
  volByMode[outMode] = volumePercent;
  prefs.putInt("vs", volByMode[0]);
  prefs.putInt("vp", volByMode[1]);
  prefs.putInt("om", outMode);
  prefs.putInt("br", opt.brightness);
  prefs.putInt("so", opt.screenOffIdx);
  prefs.putInt("sh", opt.shake);
  prefs.putInt("rp", opt.repeat);
  prefs.putInt("sf", opt.shuffle);
  prefs.putInt("vz", opt.visual);
  prefs.putInt("th", themeIndex);
  for (int i = 0; i < EQ_BANDS; ++i) {
    char key[3] = {'g', (char)('0' + i), 0};
    prefs.putInt(key, eqGain[i]);
  }
  prefs.end();
  settingsDirty = false;
}

const char *currentPresetName() {
  for (int p = 0; p < NUM_PRESETS; ++p) {
    bool same = true;
    for (int i = 0; i < EQ_BANDS; ++i) {
      if (eqGain[i] != PRESETS[p][i]) { same = false; break; }
    }
    if (same) return PRESET_NAMES[p];
  }
  return "PERSONAL";
}

void applyPreset(int p) {
  if (p < 0 || p >= NUM_PRESETS) return;
  for (int i = 0; i < EQ_BANDS; ++i) eqGain[i] = PRESETS[p][i];
  configureEqualizer();
  markSettingsDirty();
}

// ============================================================ metadados
struct Meta {
  char title[64];
  char artist[40];
  uint32_t durationSec;
  uint32_t audioStart;
  uint32_t fileSize;
};
Meta meta;
char currentTitle[110] = {0};

// Capa JPEG embutida em ID3/APIC. Limite intencional para proteger a RAM.
constexpr size_t COVER_MAX_BYTES = 24 * 1024;
static uint8_t gCoverBuf[COVER_MAX_BYTES];
uint32_t gCoverLen = 0;
bool coverReady = false;
bool coverIsPng = false;
bool coverCanvasReady = false;

static uint8_t gProbeBuf[2048];
static uint8_t gTextBuf[200];

uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint32_t syncsafe(const uint8_t *p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

// Extrai apenas JPEG de capa frontal (picture type 3); formatos/imagens grandes
// ou inválidos são ignorados sem afetar a reprodução.
bool extractJpegCover(File &f, uint32_t framePos, uint32_t frameSize) {
  if (frameSize < 12 || !f.seek(framePos)) return false;
  uint8_t encoding = 0;
  if (f.read(&encoding, 1) != 1) return false;
  uint32_t used = 1;

  // MIME string terminada em NUL.
  bool mimeEnd = false;
  while (used < frameSize) {
    const int c = f.read();
    if (c < 0) return false;
    ++used;
    if (c == 0) { mimeEnd = true; break; }
  }
  if (!mimeEnd || used >= frameSize) return false;

  uint8_t pictureType = 0;
  if (f.read(&pictureType, 1) != 1) return false;
  ++used;
  if (pictureType != 3) return false;

  // Descricao termina em 1 byte (Latin-1/UTF-8) ou NUL UTF-16.
  bool descEnd = false;
  if (encoding == 1 || encoding == 2) {
    while (used + 1 < frameSize) {
      uint8_t a = 0, b = 0;
      if (f.read(&a, 1) != 1 || f.read(&b, 1) != 1) return false;
      used += 2;
      if (a == 0 && b == 0) { descEnd = true; break; }
    }
  } else {
    while (used < frameSize) {
      const int c = f.read();
      if (c < 0) return false;
      ++used;
      if (c == 0) { descEnd = true; break; }
    }
  }
  if (!descEnd || used >= frameSize) return false;

  const uint32_t imageSize = frameSize - used;
  if (imageSize < 4 || imageSize > COVER_MAX_BYTES) return false;
  if (f.read(gCoverBuf, imageSize) != imageSize) return false;
  const bool jpeg = imageSize >= 4 && gCoverBuf[0] == 0xFF && gCoverBuf[1] == 0xD8;
  const bool png = imageSize >= 8 && gCoverBuf[0] == 0x89 && gCoverBuf[1] == 'P' &&
                   gCoverBuf[2] == 'N' && gCoverBuf[3] == 'G' &&
                   gCoverBuf[4] == 0x0D && gCoverBuf[5] == 0x0A &&
                   gCoverBuf[6] == 0x1A && gCoverBuf[7] == 0x0A;
  if (!jpeg && !png) return false;
  coverIsPng = png;
  gCoverLen = imageSize;
  coverReady = true;
  return true;
}

void probeMeta(const char *path, Meta &m) {
  memset(&m, 0, sizeof(m));
  coverReady = false;
  gCoverLen = 0;
  coverIsPng = false;
  File f = SD.open(path, FILE_READ);
  if (!f) return;
  m.fileSize = (uint32_t)f.size();

  uint32_t audioStart = 0;
  bool haveTitle = false, haveArtist = false;

  uint8_t h[10];
  if (f.read(h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
    const uint8_t ver = h[3];
    const uint32_t tagSize = syncsafe(h + 6);
    const uint32_t tagEnd = 10 + tagSize;
    audioStart = tagEnd + ((h[5] & 0x10) ? 10 : 0);
    const size_t hdr = (ver == 2) ? 6 : 10;
    uint32_t pos = 10;
    for (int guard = 0; guard < 120 && pos + hdr <= tagEnd &&
         !(haveTitle && haveArtist && coverReady); ++guard) {
      if (!f.seek(pos)) break;
      uint8_t fh[10];
      if ((size_t)f.read(fh, hdr) != hdr) break;
      char id[5] = {0};
      uint32_t fsz;
      if (ver == 2) {
        memcpy(id, fh, 3);
        fsz = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
      } else {
        memcpy(id, fh, 4);
        fsz = (ver == 4) ? syncsafe(fh + 4) : be32(fh + 4);
      }
      if (id[0] == 0 || fsz == 0 || pos + hdr + fsz > tagEnd) break;
      const bool isT = (ver == 2) ? !strcmp(id, "TT2") : !strcmp(id, "TIT2");
      const bool isA = (ver == 2) ? !strcmp(id, "TP1") : !strcmp(id, "TPE1");
      const bool isCover = (ver >= 3) && !strcmp(id, "APIC");
      if (isCover && !coverReady) extractJpegCover(f, pos + hdr, fsz);
      if ((isT && !haveTitle) || (isA && !haveArtist)) {
        const size_t take = fsz < sizeof(gTextBuf) ? fsz : sizeof(gTextBuf);
        if ((size_t)f.read(gTextBuf, take) == take && take > 1) {
          if (isT) {
            decodeText(gTextBuf + 1, take - 1, gTextBuf[0], m.title, sizeof(m.title));
            haveTitle = m.title[0] != 0;
          } else {
            decodeText(gTextBuf + 1, take - 1, gTextBuf[0], m.artist, sizeof(m.artist));
            haveArtist = m.artist[0] != 0;
          }
        }
      }
      pos += hdr + fsz;
    }
  }

  // Primeiro frame de audio e cabecalho Xing/VBRI
  m.audioStart = audioStart;
  uint32_t frames = 0, bitrate = 0, sr = 0, spf = 1152;
  if (f.seek(audioStart)) {
    const size_t got = f.read(gProbeBuf, sizeof(gProbeBuf));
    static const uint16_t BR1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t BR2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t SR1[4] = {44100, 48000, 32000, 0};
    static const uint32_t SR2[4] = {22050, 24000, 16000, 0};
    static const uint32_t SR25[4] = {11025, 12000, 8000, 0};
    for (size_t i = 0; i + 4 <= got; ++i) {
      if (gProbeBuf[i] != 0xFF || (gProbeBuf[i + 1] & 0xE0) != 0xE0) continue;
      const uint8_t b1 = gProbeBuf[i + 1], b2 = gProbeBuf[i + 2], b3 = gProbeBuf[i + 3];
      const int verBits = (b1 >> 3) & 3, layerBits = (b1 >> 1) & 3;
      const int brIdx = (b2 >> 4) & 0xF, srIdx = (b2 >> 2) & 3;
      if (verBits == 1 || layerBits != 1 || brIdx == 0 || brIdx == 15 || srIdx == 3) continue;
      const bool mpeg1 = (verBits == 3);
      bitrate = mpeg1 ? BR1[brIdx] : BR2[brIdx];
      sr = mpeg1 ? SR1[srIdx] : (verBits == 2 ? SR2[srIdx] : SR25[srIdx]);
      spf = mpeg1 ? 1152 : 576;
      const bool mono = ((b3 >> 6) & 3) == 3;
      const bool crc = !(b1 & 1);
      const size_t side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
      const size_t xoff = i + 4 + (crc ? 2 : 0) + side;
      if (xoff + 12 <= got && (!memcmp(gProbeBuf + xoff, "Xing", 4) || !memcmp(gProbeBuf + xoff, "Info", 4))) {
        if (be32(gProbeBuf + xoff + 4) & 1) frames = be32(gProbeBuf + xoff + 8);
      } else if (i + 36 + 18 <= got && !memcmp(gProbeBuf + i + 36, "VBRI", 4)) {
        frames = be32(gProbeBuf + i + 36 + 14);
      }
      m.audioStart = audioStart + (uint32_t)i;
      break;
    }
  }

  // ID3v1 (fallback de tags e fim real dos dados)
  uint32_t dataEnd = m.fileSize;
  if (m.fileSize > 128 && f.seek(m.fileSize - 128)) {
    uint8_t t[128];
    if (f.read(t, 128) == 128 && t[0] == 'T' && t[1] == 'A' && t[2] == 'G') {
      dataEnd = m.fileSize - 128;
      if (!haveTitle) decodeText(t + 3, 30, 0, m.title, sizeof(m.title));
      if (!haveArtist) decodeText(t + 33, 30, 0, m.artist, sizeof(m.artist));
    }
  }
  f.close();

  uint32_t dur = 0;
  if (frames > 0 && sr > 0) dur = (uint32_t)((uint64_t)frames * spf / sr);
  else if (bitrate > 0 && dataEnd > m.audioStart)
    dur = (uint32_t)((uint64_t)(dataEnd - m.audioStart) * 8 / ((uint64_t)bitrate * 1000));
  if (dur > 36000) dur = 0;
  m.durationSec = dur;
  if (dataEnd > m.audioStart) m.fileSize = dataEnd;  // para calculo de busca
}

void renderCoverToCache() {
  if (!coverCanvasReady) return;
  coverCanvas.fillSprite(C_BG);
  if (!coverReady) return;
  if (coverIsPng) coverCanvas.drawPng(gCoverBuf, gCoverLen, 0, 0, 68, 46);
  else coverCanvas.drawJpg(gCoverBuf, gCoverLen, 0, 0, 68, 46);
}

// ============================================================ navegador
struct Entry {
  char name[NAME_SIZE];
  bool isDir;
};
Entry entries[MAX_ENTRIES];
size_t entryCount = 0;
char browseDir[DIR_SIZE] = "/";
int selected = 0;
int firstVisible = 0;
bool scanning = false;
bool validatingDirs = false;
bool scanOverflow = false;
bool scanFolderLimit = false;
File scanHandle;

// DFS incremental para exibir apenas diretórios que contenham MP3, inclusive
// em subpastas. O limite de profundidade/itens evita loops em cartões ruins.
constexpr int DIR_WALK_DEPTH = 16;
constexpr uint32_t DIR_WALK_MAX_ITEMS = 20000;
struct WalkFrame { File dir; char path[PATH_SIZE]; };
WalkFrame walkStack[DIR_WALK_DEPTH];
int walkDepth = 0;
size_t validationIndex = 0;
uint32_t walkItems = 0;
bool walkFoundSong = false;
bool dirProbeActive = false;
char walkRootPath[PATH_SIZE] = "/";
char reselectName[NAME_SIZE] = {0};

// fila de reproducao (copia da pasta no momento do ENTER)
char queueDir[DIR_SIZE] = "/";
char queueNames[MAX_ENTRIES][NAME_SIZE];
uint16_t order[MAX_ENTRIES];
size_t queueCount = 0;
size_t orderPos = 0;

int cmpEntry(const Entry &a, const Entry &b) {
  if (a.isDir != b.isDir) return a.isDir ? -1 : 1;
  return strcasecmp(a.name, b.name);
}

void ensureVisible() {
  if (selected < firstVisible) firstVisible = selected;
  if (selected >= firstVisible + ROWS) firstVisible = selected - ROWS + 1;
  if (firstVisible < 0) firstVisible = 0;
}

void addEntry(File &e) {
  if (entryCount >= MAX_ENTRIES) return;
  const char *raw = e.name();
  if (!raw) return;
  const char *slash = strrchr(raw, '/');
  const char *base = slash ? slash + 1 : raw;
  if (!base[0] || base[0] == '.') return;
  const bool dir = e.isDirectory();
  if (!dir && !hasMp3Extension(base)) return;
  if (dir && strcasecmp(base, "System Volume Information") == 0) return;
  if (strlen(base) >= NAME_SIZE) return;

  Entry tmp;
  safeCopy(tmp.name, NAME_SIZE, base);
  tmp.isDir = dir;
  size_t pos = entryCount;
  while (pos > 0 && cmpEntry(entries[pos - 1], tmp) > 0) --pos;
  memmove(&entries[pos + 1], &entries[pos], (entryCount - pos) * sizeof(Entry));
  entries[pos] = tmp;
  ++entryCount;
}

void closeWalkStack() {
  while (walkDepth > 0) {
    --walkDepth;
    if (walkStack[walkDepth].dir) walkStack[walkDepth].dir.close();
  }
}

bool startDirValidation(size_t index) {
  dirProbeActive = false;
  if (index >= entryCount || !entries[index].isDir) return false;
  if (!buildPath(walkRootPath, sizeof(walkRootPath), browseDir, entries[index].name))
    return false;
  File root = SD.open(walkRootPath);
  if (!root || !root.isDirectory()) { if (root) root.close(); return false; }
  safeCopy(walkStack[0].path, sizeof(walkStack[0].path), walkRootPath);
  walkStack[0].dir = root;
  walkDepth = 1;
  walkItems = 0;
  walkFoundSong = false;
  dirProbeActive = true;
  return true;
}

void walkDirectoryStep() {
  if (walkDepth <= 0) return;
  File &parent = walkStack[walkDepth - 1].dir;
  File child = parent.openNextFile();
  if (!child) {
    parent.close();
    --walkDepth;
    return;
  }

  if (++walkItems > DIR_WALK_MAX_ITEMS) {
    // Não marca como música uma pasta que não foi verificada por completo.
    child.close();
    scanFolderLimit = true;
    walkFoundSong = false;
    closeWalkStack();
    return;
  }

  const char *raw = child.name();
  const char *slash = raw ? strrchr(raw, '/') : nullptr;
  const char *base = slash ? slash + 1 : raw;
  if (!base || !base[0] || base[0] == '.') { child.close(); return; }

  if (!child.isDirectory()) {
    const bool found = hasMp3Extension(base);
    child.close();
    if (found) {
      walkFoundSong = true;
      closeWalkStack();
    }
    return;
  }

  if (strcasecmp(base, "System Volume Information") == 0) { child.close(); return; }
  char childPath[PATH_SIZE];
  if (!buildPath(childPath, sizeof(childPath), walkStack[walkDepth - 1].path, base)) {
    child.close();
    scanFolderLimit = true;
    walkFoundSong = false;
    closeWalkStack();
    return;
  }
  child.close();
  if (walkDepth >= DIR_WALK_DEPTH) {
    scanFolderLimit = true;
    walkFoundSong = false;
    closeWalkStack();
    return;
  }
  File next = SD.open(childPath);
  if (!next || !next.isDirectory()) { if (next) next.close(); return; }
  safeCopy(walkStack[walkDepth].path, sizeof(walkStack[walkDepth].path), childPath);
  walkStack[walkDepth].dir = next;
  ++walkDepth;
}

void finishScan() {
  if (scanHandle) scanHandle.close();
  closeWalkStack();
  scanning = false;
  validatingDirs = false;
  dirProbeActive = false;
  if (entryCount > 0) selected = constrain(selected, 0, (int)entryCount - 1);
  else selected = firstVisible = 0;
  if (reselectName[0]) {
    for (size_t i = 0; i < entryCount; ++i) {
      if (entries[i].isDir && strcmp(entries[i].name, reselectName) == 0) {
        selected = (int)i;
        break;
      }
    }
    reselectName[0] = '\0';
    ensureVisible();
  }
  if (scanFolderLimit) showToast("PASTA GRANDE: FILTRO PARCIAL");
  else if (scanOverflow) showToast("LIMITE: 200 ITENS");
  if (screen == SCR_BROWSER) drawBrowser();
}

void beginScan(const char *path, const char *reselect = nullptr) {
  if (scanning) { if (scanHandle) scanHandle.close(); scanning = false; }
  closeWalkStack();
  validatingDirs = false;
  dirProbeActive = false;
  validationIndex = 0;
  entryCount = 0;
  selected = 0;
  firstVisible = 0;
  scanOverflow = false;
  scanFolderLimit = false;
  if (strcmp(path, browseDir) != 0) safeCopy(browseDir, sizeof(browseDir), path);
  safeCopy(reselectName, sizeof(reselectName), reselect ? reselect : "");

  if (!sdReady) { if (screen == SCR_BROWSER) drawBrowser(); return; }
  scanHandle = SD.open(browseDir);
  if (!scanHandle || !scanHandle.isDirectory()) {
    if (scanHandle) scanHandle.close();
    showToast("PASTA INVALIDA");
    if (screen == SCR_BROWSER) drawBrowser();
    return;
  }
  scanning = true;
  if (screen == SCR_BROWSER) drawBrowser();
}

// Le poucos itens por chamada; a listagem e a busca recursiva cedem tempo ao audio.
void scanStep(uint32_t budgetMs) {
  if (!scanning) return;
  const uint32_t t0 = millis();
  while (millis() - t0 < budgetMs) {
    if (validatingDirs) {
      if (dirProbeActive) {
        if (walkDepth > 0) { walkDirectoryStep(); continue; }

        // O probe terminou: conserva o diretório se achou MP3, senão remove.
        dirProbeActive = false;
        if (walkFoundSong) {
          ++validationIndex;
        } else if (validationIndex < entryCount) {
          memmove(&entries[validationIndex], &entries[validationIndex + 1],
                  (entryCount - validationIndex - 1) * sizeof(Entry));
          --entryCount;
        }
        walkFoundSong = false;
      }

      while (validationIndex < entryCount && !entries[validationIndex].isDir)
        ++validationIndex;
      if (validationIndex >= entryCount) { finishScan(); return; }
      if (!startDirValidation(validationIndex)) {
        // Se a pasta sumiu ou ficou inacessível, remove só ela e busca a próxima.
        memmove(&entries[validationIndex], &entries[validationIndex + 1],
                (entryCount - validationIndex - 1) * sizeof(Entry));
        --entryCount;
      }
      continue;
    }

    File e = scanHandle.openNextFile();
    if (!e || entryCount >= MAX_ENTRIES) {
      if (e) e.close();
      if (entryCount >= MAX_ENTRIES) scanOverflow = true;
      if (scanHandle) scanHandle.close();
      validatingDirs = true;
      dirProbeActive = false;
      validationIndex = 0;
      walkDepth = 0;
      walkFoundSong = false;
      continue;
    }
    addEntry(e);
    e.close();
  }
}

void enterDir(const char *name) {
  char path[PATH_SIZE];
  if (!buildPath(path, sizeof(path), browseDir, name) || strlen(path) >= sizeof(browseDir)) {
    showToast("CAMINHO LONGO");
    drawBrowser();
    return;
  }
  beginScan(path);
}

void parentDir() {
  if (strcmp(browseDir, "/") == 0) return;
  char *slash = strrchr(browseDir, '/');
  if (!slash) return;
  char name[NAME_SIZE];
  safeCopy(name, sizeof(name), slash + 1);
  char parent[DIR_SIZE];
  if (slash == browseDir) {
    parent[0] = '/'; parent[1] = '\0';
  } else {
    const size_t len = (size_t)(slash - browseDir);
    memcpy(parent, browseDir, len);
    parent[len] = '\0';
  }
  beginScan(parent, name);
}

void moveSelection(int delta, bool wrap) {
  if (!entryCount) return;
  selected += delta;
  if (wrap) {
    if (selected < 0) selected = (int)entryCount - 1;
    if (selected >= (int)entryCount) selected = 0;
  } else {
    selected = constrain(selected, 0, (int)entryCount - 1);
  }
  ensureVisible();
  drawBrowser();
}

// ============================================================ fila / ordem
void shuffleOrder(int first) {
  const int n = (int)queueCount;
  for (int i = 0; i < n; ++i) order[i] = (uint16_t)i;
  for (int i = n - 1; i > 0; --i) {
    const int j = (int)random(i + 1);
    const uint16_t t = order[i]; order[i] = order[j]; order[j] = t;
  }
  if (first >= 0) {
    for (int i = 0; i < n; ++i) {
      if (order[i] == first) { order[i] = order[0]; order[0] = (uint16_t)first; break; }
    }
  }
  orderPos = 0;
}

void rebuildOrder(int currentQ) {
  const int n = (int)queueCount;
  if (!n) { orderPos = 0; return; }
  if (opt.shuffle && n > 1) {
    shuffleOrder(currentQ);
  } else {
    for (int i = 0; i < n; ++i) order[i] = (uint16_t)i;
    orderPos = (size_t)constrain(currentQ, 0, n - 1);
  }
}

int buildQueueFromBrowser(int entryIdx) {
  queueCount = 0;
  int chosen = -1;
  for (size_t i = 0; i < entryCount && queueCount < MAX_ENTRIES; ++i) {
    if (entries[i].isDir) continue;
    if ((int)i == entryIdx) chosen = (int)queueCount;
    safeCopy(queueNames[queueCount], NAME_SIZE, entries[i].name);
    ++queueCount;
  }
  safeCopy(queueDir, sizeof(queueDir), browseDir);
  return chosen;
}

// ============================================================ reproducao
uint32_t startOffsetSec = 0;
uint32_t lastProgress = 0;
uint32_t lastSamples = 0;

uint32_t elapsedSec() {
  return startOffsetSec + (uint32_t)(gSamplesSubmitted / (uint32_t)gRate);
}

void stopPlayback(bool cutAudio) {
  if (cutAudio) {
    M5Cardputer.Speaker.stop();
    gFill = 0;
  }
  if (decoder) {
    if (decoder->isRunning()) decoder->stop();
    delete decoder;
    decoder = nullptr;
  }
  if (bufferedSource) { delete bufferedSource; bufferedSource = nullptr; }
  if (source) { source->close(); delete source; source = nullptr; }
  if (audioOutput) { delete audioOutput; audioOutput = nullptr; }
  playing = false;
  paused = false;
  fadeAction = FA_NONE;
  memset(gBand, 0, sizeof(gBand));
  memset(gScope, 0, sizeof(gScope));
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  neopixelWrite(21, 0, 0, 0);
#endif
}

bool startTrack(size_t qIdx, uint32_t startSec, bool reprobe, bool natural) {
  if (qIdx >= queueCount) return false;
  char path[PATH_SIZE];
  if (!buildPath(path, sizeof(path), queueDir, queueNames[qIdx])) return false;

  stopPlayback(!natural);

  if (reprobe) {
    probeMeta(path, meta);
    renderCoverToCache();
    char base[NAME_SIZE];
    toAscii(queueNames[qIdx], base, sizeof(base));
    const size_t bl = strlen(base);
    if (bl > 4) base[bl - 4] = '\0';
    if (meta.title[0] && meta.artist[0]) snprintf(currentTitle, sizeof(currentTitle), "%s - %s", meta.artist, meta.title);
    else if (meta.title[0]) safeCopy(currentTitle, sizeof(currentTitle), meta.title);
    else if (meta.artist[0]) snprintf(currentTitle, sizeof(currentTitle), "%s - %s", meta.artist, base);
    else safeCopy(currentTitle, sizeof(currentTitle), base);
  }

  uint32_t startByte = meta.audioStart;  // pula tag ID3 (evita falso sincronismo)
  uint32_t startedAt = 0;
  if (startSec > 0 && meta.durationSec > 1 && meta.fileSize > meta.audioStart) {
    if (startSec >= meta.durationSec) startSec = meta.durationSec - 1;
    const uint64_t span = meta.fileSize - meta.audioStart;
    startByte = meta.audioStart + (uint32_t)(span * startSec / meta.durationSec);
    startedAt = startSec;
  }

  source = new (std::nothrow) AudioFileSourceSD();
  if (!source || !source->open(path)) { stopPlayback(true); return false; }
  if (startByte > 0) source->seek((int32_t)startByte, SEEK_SET);

  bufferedSource = new (std::nothrow) AudioFileSourceBuffer(source, sdAudioBuffer, sizeof(sdAudioBuffer));
  audioOutput = new (std::nothrow) Mp3AudioOutput();
  decoder = new (std::nothrow) AudioGeneratorMP3();
  if (!bufferedSource || !audioOutput || !decoder) { stopPlayback(true); return false; }

  audioOutput->SetGain(1.0f);
  gFadeTarget = 1.0f;
  gFadeGain = natural ? 1.0f : 0.0f;  // fade-in curto, exceto na troca gapless
  if (!natural) { resetEq(); gVolCur = gVolTarget; }
  gSamplesSubmitted = 0;

  if (!decoder->begin(bufferedSource, audioOutput)) { stopPlayback(true); return false; }

  startOffsetSec = startedAt;
  lastProgress = millis();
  lastSamples = 0;
  playing = true;
  paused = false;
  fadeAction = FA_NONE;
  return true;
}

void finishQueue() {
  stopPlayback(false);  // deixa o final da ultima faixa terminar
  showToast("FIM DA LISTA");
  if (screen == SCR_PLAYER) { screen = SCR_BROWSER; }
  redrawCurrent();
}

// Avanca na ordem de reproducao. automatic=true: fim natural da faixa.
bool advance(int dir, bool automatic) {
  if (!queueCount) return false;
  const int n = (int)queueCount;
  int pos = (int)orderPos;
  for (int tries = 0; tries < n; ++tries) {
    int np = pos + dir;
    if (np >= n) {
      if (automatic && opt.repeat == 0) { finishQueue(); return false; }
      np = 0;
      if (opt.shuffle && n > 1) {
        const uint16_t last = order[pos];
        shuffleOrder(-1);
        if (order[0] == last) { const uint16_t t = order[0]; order[0] = order[1]; order[1] = t; }
      }
    }
    if (np < 0) np = n - 1;
    pos = np;
    orderPos = (size_t)pos;
    if (startTrack(order[pos], 0, true, automatic)) { redrawCurrent(); return true; }
  }
  stopPlayback(true);
  showToast("ERRO AO ABRIR MP3");
  if (screen == SCR_PLAYER) screen = SCR_BROWSER;
  redrawCurrent();
  return false;
}

void onTrackEnded() {
  if (opt.repeat == 2) {
    if (startTrack(order[orderPos], 0, false, true)) return;
  }
  advance(+1, true);
}

void previousOrRestart() {
  if (!playing) return;
  if (elapsedSec() > 3) {
    if (startTrack(order[orderPos], 0, false, false)) redrawCurrent();
    else advance(+1, true);
  } else {
    advance(-1, false);
  }
}

void seekBy(int delta) {
  if (!playing) return;
  if (meta.durationSec < 3) { showToast("BUSCA INDISPONIVEL"); return; }
  int32_t target = (int32_t)elapsedSec() + delta;
  if (target < 0) target = 0;
  if (target >= (int32_t)meta.durationSec - 1) { advance(+1, false); return; }
  if (startTrack(order[orderPos], (uint32_t)target, false, false)) redrawCurrent();
  else advance(+1, true);
}

void togglePause() {
  if (!playing) return;
  if (paused) {
    paused = false;
    fadeAction = FA_NONE;
    gFadeTarget = 1.0f;
    lastProgress = millis();
  } else if (fadeAction == FA_PAUSE) {
    fadeAction = FA_NONE;  // cancela pausa em andamento
    gFadeTarget = 1.0f;
  } else {
    fadeAction = FA_PAUSE;
    gFadeTarget = 0.0f;
  }
  redrawCurrent();
}

void applyShuffleChange() {
  if (!queueCount) return;
  rebuildOrder((int)order[orderPos < queueCount ? orderPos : 0]);
}

void setSleepTimer() {
  sleepDeadline = SLEEP_MIN[sleepIdx] ? (millis() + (uint32_t)SLEEP_MIN[sleepIdx] * 60000UL) : 0;
}

void changeVolume(int delta) {
  volumePercent = constrain(volumePercent + delta, 0, 100);
  volByMode[outMode] = volumePercent;
  updateVolumeTarget();
  markSettingsDirty();
  char t[24];
  snprintf(t, sizeof(t), "VOLUME %d%%", volumePercent);
  showToast(t);
  redrawCurrent();
}

// ============================================================ desenho
void drawHeader(LovyanGFX &g, const char *title, uint16_t titleColor) {
  g.fillRect(0, 0, 240, 20, C_DARKRED);
  g.fillRect(0, 20, 240, 1, C_RED);
  g.setTextSize(1);
  g.setFont(&fonts::Font2);
  g.setTextColor(C_RED);
  g.drawString("MP3 PLAYER", 8, 2);

  g.setFont(&fonts::Font0);
  if (batteryPercent >= 0 && batteryPercent <= 100) {
    char b[12];
    snprintf(b, sizeof(b), "BAT %d%%", batteryPercent);
    g.setTextColor(batteryPercent <= 15 ? C_RED : C_GREY);
    g.drawString(b, 66, 6);
  }
  char fit[40];
  fitText(g, title, 100, fit, sizeof(fit));
  g.setTextColor(titleColor);
  g.drawString(fit, 232 - g.textWidth(fit), 6);
}

void drawSplash() {
  auto &d = M5Cardputer.Display;
  d.fillScreen(C_BG);
  d.setFont(&fonts::Font2);
  d.setTextSize(2);
  d.setTextColor(C_RED);
  const char *l1 = "MP3 PLAYER";
  d.drawString(l1, (240 - d.textWidth(l1)) / 2, 35);
  d.setTextSize(1);
  d.drawFastHLine(18, 82, 204, C_RED);
  d.setFont(&fonts::Font0);
  d.setTextColor(C_GREY);
  char v[28];
  snprintf(v, sizeof(v), "CARDPUTER ADV  |  %s", MP3_PLAYER_VERSION);
  d.drawString(v, (240 - d.textWidth(v)) / 2, 94);
}

void formatTime(char *out, size_t n, uint32_t secs) {
  snprintf(out, n, "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
}

float progressFraction() {
  if (meta.durationSec > 0) {
    float f = (float)elapsedSec() / (float)meta.durationSec;
    return constrain(f, 0.0f, 1.0f);
  }
  if (source && source->getSize() > 0) {
    float f = (float)source->getPos() / (float)source->getSize();
    return constrain(f, 0.0f, 1.0f);
  }
  return 0.0f;
}

void drawBrowser() {
  screen = SCR_BROWSER;
  if (screenOff) return;
  auto &g = ui;
  g.fillSprite(C_BG);

  char title[NAME_SIZE];
  if (browseDir[1] == '\0') safeCopy(title, sizeof(title), "SD CARD");
  else {
    const char *base = strrchr(browseDir, '/');
    toAscii(base ? base + 1 : browseDir, title, sizeof(title));
  }
  drawHeader(g, title, C_CYAN);

  g.setFont(&fonts::Font2);
  g.setTextSize(1);
  constexpr int ROW_H = 17, Y0 = 24;

  if (!sdReady) {
    g.setTextColor(C_RED);
    g.drawString("MICROSD NAO ENCONTRADO", 14, 40);
    g.setFont(&fonts::Font0);
    g.setTextColor(C_GREY);
    g.drawString("COLOQUE O CARTAO E TECLE R", 14, 62);
  } else if (scanning) {
    g.setTextColor(C_GREY);
    char s[32];
    snprintf(s, sizeof(s), validatingDirs ? "FILTRANDO PASTAS %d" : "LENDO PASTA... %d", (int)entryCount);
    g.drawString(s, 14, 44);
  } else if (!entryCount) {
    g.setTextColor(C_GREY);
    g.drawString("PASTA VAZIA (SEM MP3)", 14, 44);
  }

  if (!scanning) {
    for (int r = 0; r < ROWS; ++r) {
      const int idx = firstVisible + r;
      if (idx >= (int)entryCount) break;
      const int y = Y0 + r * ROW_H;
      const bool active = (idx == selected);
      const Entry &e = entries[idx];
      if (active) {
        g.fillRoundRect(6, y, 224, 16, 3, C_PANEL);
        g.fillRect(6, y + 2, 3, 12, C_RED);
      }
      const uint16_t ic = e.isDir ? C_YELLOW : C_CYAN;
      if (e.isDir) {
        g.fillRect(13, y + 4, 6, 2, ic);
        g.fillRect(13, y + 6, 13, 8, ic);
      } else {
        g.fillCircle(16, y + 11, 3, ic);
        g.drawFastVLine(18, y + 3, 9, ic);
        g.drawLine(18, y + 3, 22, y + 6, ic);
      }
      char shown[NAME_SIZE], fit[NAME_SIZE];
      toAscii(e.name, shown, sizeof(shown));
      if (!e.isDir) {
        const size_t sl = strlen(shown);
        if (sl > 4) shown[sl - 4] = '\0';
      }
      g.setFont(&fonts::Font2);
      fitText(g, shown, 192, fit, sizeof(fit));
      g.setTextColor(active ? C_WHITE : C_GREY);
      g.drawString(fit, 32, y);
    }
    if ((int)entryCount > ROWS) {
      const int trackH = ROWS * ROW_H;
      g.fillRect(234, Y0, 3, trackH, C_DIM);
      int thumbH = trackH * ROWS / (int)entryCount;
      if (thumbH < 8) thumbH = 8;
      const int thumbY = Y0 + (trackH - thumbH) * firstVisible / ((int)entryCount - ROWS);
      g.fillRect(234, thumbY, 3, thumbH, C_RED);
    }
  }

  // painel inferior: mini player ou dicas
  g.fillRect(0, 94, 240, 41, C_PANEL2);
  g.fillRect(0, 94, 240, 1, C_DIM);
  if (playing) {
    char fit[110];
    g.setFont(&fonts::Font2);
    fitText(g, currentTitle, 224, fit, sizeof(fit));
    g.setTextColor(C_WHITE);
    g.drawString(fit, 8, 97);
    g.fillRect(8, 116, 224, 3, C_DIM);
    g.fillRect(8, 116, (int)(224 * progressFraction()), 3, C_RED);
    g.setFont(&fonts::Font0);
    char t[12], d[12], line[32];
    formatTime(t, sizeof(t), elapsedSec());
    if (meta.durationSec) { formatTime(d, sizeof(d), meta.durationSec); snprintf(line, sizeof(line), "%s / %s", t, d); }
    else safeCopy(line, sizeof(line), t);
    g.setTextColor(C_CYAN);
    g.drawString(line, 8, 125);
    const char *st = (paused || fadeAction == FA_PAUSE) ? "PAUSADO" : "TOCANDO";
    g.setTextColor((paused || fadeAction == FA_PAUSE) ? C_YELLOW : C_GREY);
    g.drawString(st, 232 - g.textWidth(st), 125);
  } else {
    g.setFont(&fonts::Font0);
    g.setTextColor(C_GREY);
    g.drawString("ENTER ABRE/TOCA   DEL PASTA ACIMA", 8, 101);
    g.drawString("S AJUSTES  E EQ  R ATUALIZA  H AJUDA", 8, 113);
    if (entryCount) {
      char c[20];
      snprintf(c, sizeof(c), "%d/%d", selected + 1, (int)entryCount);
      g.setTextColor(C_CYAN);
      g.drawString(c, 232 - g.textWidth(c), 125);
    }
  }
  if (toastActive && millis() < toastUntil) {
    g.fillRect(0, 122, 240, 13, C_PANEL2);
    g.setFont(&fonts::Font0);
    g.setTextColor(C_YELLOW);
    g.drawString(toastText, 8, 125);
  }
  ui.pushSprite(0, 0);
}

void drawLive() {
  if (screenOff) return;
  auto &g = live;
  g.fillSprite(C_BG);

  g.setFont(&fonts::Font2);
  g.setTextSize(1);
  g.setTextColor(C_WHITE);
  g.setClipRect(8, 0, 224, 17);
  const int tw = g.textWidth(currentTitle);
  if (tw <= 224) {
    g.drawString(currentTitle, 8, 0);
  } else {
    const int span = tw + 40;
    const int off = (int)((millis() / 28) % (uint32_t)span);
    g.drawString(currentTitle, 8 - off, 0);
    g.drawString(currentTitle, 8 - off + span, 0);
  }
  g.clearClipRect();

  g.setFont(&fonts::Font0);
  if (toastActive && millis() < toastUntil) {
    g.setTextColor(C_YELLOW);
    g.drawString(toastText, 8, 18);
  } else {
    g.setTextColor(C_GREY);
    char sub[24];
    snprintf(sub, sizeof(sub), "FAIXA %d/%d", (int)orderPos + 1, (int)queueCount);
    g.drawString(sub, 8, 18);
    char flags[16] = "";
    if (opt.shuffle) strcat(flags, "SHUF ");
    if (opt.repeat == 1) strcat(flags, "REP");
    else if (opt.repeat == 2) strcat(flags, "REP1");
    g.setTextColor(C_YELLOW);
    g.drawString(flags, 86, 18);
    if (sleepDeadline) {
      const int32_t rem = (int32_t)(sleepDeadline - millis());
      char z[12];
      snprintf(z, sizeof(z), "ZZ %dM", (int)(rem > 0 ? rem / 60000 + 1 : 0));
      g.setTextColor(C_ORANGE);
      g.drawString(z, 150, 18);
    }
    char rate[16];
    snprintf(rate, sizeof(rate), "%.1fK", gRate / 1000.0f);
    g.setTextColor(C_GREY);
    g.drawString(rate, 232 - g.textWidth(rate), 18);
  }

  static float shown[SPEC_BANDS] = {0};
  static float peak[SPEC_BANDS] = {0};
  constexpr int BAR_W = 14, BAR_GAP = 5, SEGS = 10, SEG_H = 3, TOP = 30;
  const bool active = playing && !paused;

  if (opt.visual == 3) {
    g.drawRoundRect(83, 27, 74, 52, 5, C_DIM);
    if (coverReady) {
      g.fillRect(86, 30, 68, 46, C_PANEL2);
    } else {
      g.setFont(&fonts::Font0);
      g.setTextColor(C_GREY);
      g.drawString("SEM CAPA", 91, 48);
    }
  } else if (opt.visual == 2) {
    const int cy = 48;
    g.drawFastHLine(8, cy, 224, C_DIM);
    const float gain = max(gVolCur * gPreamp * gFadeGain, 0.12f);
    const float scale = 19.0f / (12000.0f * gain);
    int px = 8, py = cy;
    for (int k = 0; k < 128; ++k) {
      const int x = 8 + k * 223 / 127;
      int y = cy - (int)((active ? gScope[k] : 0) * scale);
      y = constrain(y, cy - 19, cy + 19);
      if (k > 0) g.drawLine(px, py, x, y, C_CYAN);
      px = x; py = y;
    }
  } else {
    for (int i = 0; i < SPEC_BANDS; ++i) {
      const float lvl = active ? gBand[i] : 0.0f;
      if (lvl > shown[i]) shown[i] = lvl;
      else shown[i] = max(0.0f, shown[i] - 0.05f);
      if (shown[i] > peak[i]) peak[i] = shown[i];
      else peak[i] = max(0.0f, peak[i] - 0.012f);
      const int x = 8 + i * (BAR_W + BAR_GAP);
      if (opt.visual == 0) {
        const int lit = (int)(shown[i] * SEGS + 0.5f);
        const int peakSeg = min(SEGS - 1, (int)(peak[i] * SEGS));
        for (int s = 0; s < SEGS; ++s) {
          const int y = TOP + (SEGS - 1 - s) * (SEG_H + 1);
          uint16_t col = C_DIM;
          if (s < lit) col = (s < 5) ? C_CYAN : (s < 8 ? C_BLUE : C_MAGENTA);
          else if (s == peakSeg && peak[i] > 0.05f) col = C_WHITE;
          g.fillRect(x, y, BAR_W, SEG_H, col);
        }
      } else {
        const int cy = 49;
        const int h = (int)(shown[i] * 19.0f);
        const uint16_t col = i < 4 ? C_CYAN : (i < 8 ? C_BLUE : C_MAGENTA);
        g.fillRect(x, cy - h, BAR_W, 2 * h + 1, col);
        g.drawFastHLine(x, cy, BAR_W, C_WHITE);
      }
    }
  }

  const float frac = progressFraction();
  g.fillRect(8, 72, 224, 4, C_DIM);
  g.fillRect(8, 72, (int)(224 * frac), 4, C_RED);

  char t[12], line[32];
  formatTime(t, sizeof(t), elapsedSec());
  if (meta.durationSec) {
    char d[12];
    formatTime(d, sizeof(d), meta.durationSec);
    snprintf(line, sizeof(line), "%s / %s", t, d);
  } else {
    safeCopy(line, sizeof(line), t);
  }
  g.setTextColor(C_CYAN);
  g.drawString(line, 8, 80);
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100));
  g.setTextColor(C_GREY);
  g.drawString(pct, 232 - g.textWidth(pct), 80);

  live.pushSprite(0, 22);
  // Desenha por cima do sprite já composto; M5GFX decodifica da RAM sem
  // abrir outro arquivo no cartão enquanto o decoder de MP3 está ativo.
  if (opt.visual == 3 && coverReady && coverCanvasReady)
    coverCanvas.pushSprite(86, 52);
}

void drawPlayer() {
  screen = SCR_PLAYER;
  if (screenOff) return;
  auto &g = ui;
  g.fillSprite(C_BG);
  const bool p = paused || fadeAction == FA_PAUSE;
  drawHeader(g, p ? "PAUSADO" : "TOCANDO", p ? C_YELLOW : C_CYAN);

  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  g.setTextColor(C_GREY);
  g.drawString(outMode ? "FON" : "ALT", 8, 117);
  g.drawRect(32, 116, 102, 9, C_GREY);
  g.fillRect(33, 117, volumePercent, 7, C_RED);
  char v[8];
  snprintf(v, sizeof(v), "%d%%", volumePercent);
  g.setTextColor(C_WHITE);
  g.drawString(v, 140, 117);

  char e[24];
  snprintf(e, sizeof(e), "EQ %s", currentPresetName());
  g.setTextColor(C_CYAN);
  g.drawString(e, 232 - g.textWidth(e), 117);

  g.setTextColor(C_GREY);
  g.drawString("SPC PAUSA N/P FAIXA ,/ BUSCA TAB EQ", 8, 127);

  ui.pushSprite(0, 0);
  drawLive();
}

void drawEq() {
  screen = SCR_EQ;
  if (screenOff) return;
  auto &g = ui;
  g.fillSprite(C_BG);
  drawHeader(g, "EQUALIZADOR", C_CYAN);

  constexpr int CENTER_Y = 66, HALF = 26;
  for (int i = 0; i < EQ_BANDS; ++i) {
    const int cx = 32 + i * 44;
    const bool sel = (i == eqSelected);
    if (sel) g.drawRoundRect(cx - 20, 24, 40, 92, 4, C_RED);

    g.setFont(&fonts::Font0);
    g.setTextColor(sel ? C_WHITE : C_GREY);
    g.drawString(EQ_LABELS[i], cx - g.textWidth(EQ_LABELS[i]) / 2, 29);

    g.fillRect(cx - 1, 40, 3, 52, C_DIM);
    g.drawFastHLine(cx - 9, CENTER_Y, 19, C_GREY);
    const int gy = CENTER_Y - eqGain[i] * HALF / EQ_LIMIT;
    if (eqGain[i] > 0) g.fillRect(cx - 3, gy, 7, CENTER_Y - gy, C_CYAN);
    else if (eqGain[i] < 0) g.fillRect(cx - 3, CENTER_Y, 7, gy - CENTER_Y, C_ORANGE);
    g.fillRoundRect(cx - 8, gy - 2, 17, 5, 2, sel ? C_RED : C_WHITE);

    char gs[8];
    snprintf(gs, sizeof(gs), "%+d", eqGain[i]);
    g.setFont(&fonts::Font2);
    g.setTextColor(sel ? C_RED : C_WHITE);
    g.drawString(gs, cx - g.textWidth(gs) / 2, 96);
  }

  g.setFont(&fonts::Font0);
  g.setTextColor(C_GREY);
  g.drawString("PRESET", 8, 119);
  g.setTextColor(C_CYAN);
  g.drawString(currentPresetName(), 50, 119);
  g.setTextColor(C_GREY);
  g.drawString("1-6 PRESET  0 ZERA  TAB VOLTA", 8, 127);
  ui.pushSprite(0, 0);
}

constexpr int SETTINGS_COUNT = 9;
const char *SETTING_LABELS[SETTINGS_COUNT] = {
  "BRILHO", "TELA APAGA", "TIMER DE SONO", "AGITAR = PROXIMA",
  "REPETIR", "ALEATORIO", "VISUALIZADOR", "TEMA", "RESTAURAR PADROES"
};

void settingValue(int i, char *out, size_t n) {
  switch (i) {
    case 0: snprintf(out, n, "%d%%", opt.brightness); break;
    case 1:
      if (SCREEN_OFF_SEC[opt.screenOffIdx]) snprintf(out, n, "%ds", SCREEN_OFF_SEC[opt.screenOffIdx]);
      else safeCopy(out, n, "NUNCA");
      break;
    case 2:
      if (SLEEP_MIN[sleepIdx]) snprintf(out, n, "%d MIN", SLEEP_MIN[sleepIdx]);
      else safeCopy(out, n, "DESLIGADO");
      break;
    case 3: safeCopy(out, n, imuOk ? (opt.shake ? "LIGADO" : "DESLIGADO") : "N/D"); break;
    case 4: safeCopy(out, n, REPEAT_NAMES[opt.repeat]); break;
    case 5: safeCopy(out, n, opt.shuffle ? "LIGADO" : "DESLIGADO"); break;
    case 6: safeCopy(out, n, VISUAL_NAMES[opt.visual]); break;
    case 7: safeCopy(out, n, THEMES[themeIndex].name); break;
    default: safeCopy(out, n, "ENTER"); break;
  }
}

void drawSettings() {
  screen = SCR_SETTINGS;
  if (screenOff) return;
  auto &g = ui;
  g.fillSprite(C_BG);
  drawHeader(g, "AJUSTES", C_CYAN);
  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  for (int i = 0; i < SETTINGS_COUNT; ++i) {
    const int y = 24 + i * 11;
    const bool sel = (i == settingsSel);
    if (sel) g.fillRoundRect(6, y - 2, 228, 11, 3, C_PANEL);
    g.setTextColor(sel ? C_WHITE : C_GREY);
    g.drawString(SETTING_LABELS[i], 12, y);
    char v[24], shown[32];
    settingValue(i, v, sizeof(v));
    if (sel && i < SETTINGS_COUNT - 1) snprintf(shown, sizeof(shown), "< %s >", v);
    else safeCopy(shown, sizeof(shown), v);
    g.setTextColor(sel ? C_CYAN : C_GREY);
    g.drawString(shown, 228 - g.textWidth(shown), y);
  }
  g.setTextColor(C_GREY);
  g.drawString("; . ESCOLHE   , / MUDA   ESC VOLTA", 8, 126);
  ui.pushSprite(0, 0);
}

void drawHelp() {
  screen = SCR_HELP;
  if (screenOff) return;
  static const char *const pages[3][10] = {
    {
      "BIBLIOTECA / GERAL",
      ";/. move item   ,// pagina",
      "ENTER pasta ou comeca a fila",
      "DEL sobe pasta  TAB abre player",
      "SPACE pausa  N/P muda faixa",
      "- / = volume  1-6 preset EQ",
      "S ajustes  E equalizador",
      "R atualiza SD  X para audio",
      "H avanca o tutorial",
      "Atalhos gerais valem em todas telas",
    },
    {
      "PLAYER",
      "SPACE pausa | N proxima",
      "P anterior / reinicia (>3s)",
      ", / / busca -10s / +10s",
      "; / . volume +/- 5%",
      "S aleatorio | R off/todas/uma",
      "V barras/espelho/onda/capa",
      "T timer sono | O perfil volume",
      "TAB/E equalizador",
      "DEL ou ` volta a lista; X para",
    },
    {
      "EQ / AJUSTES",
      "EQ: , / troca banda",
      "EQ: ; / . ganho +/-",
      "EQ: 0 FLAT | 1-6 presets",
      "AJUSTES: ; / . escolhe item",
      "AJUSTES: , / / muda valor",
      "ENTER muda / restaura padroes",
      "TAB/DEL/` volta a tela anterior",
      "Tela apagada: qualquer tecla acorda",
      "H reinicia | TAB/DEL fecha ajuda",
    },
  };

  auto &g = ui;
  g.fillSprite(C_BG);
  char title[24];
  snprintf(title, sizeof(title), "TUTORIAL %d/3", helpPage + 1);
  drawHeader(g, title, C_CYAN);
  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  for (int i = 0; i < 10; ++i) {
    g.setTextColor(i == 0 ? C_RED : C_GREY);
    g.drawString(pages[helpPage][i], i == 0 ? 8 : 12, 26 + i * 10);
  }
  g.setTextColor(C_CYAN);
  g.drawString("H PROXIMA  TAB/DEL/` FECHA", 8, 127);
  ui.pushSprite(0, 0);
}

void redrawCurrent() {
  switch (screen) {
    case SCR_BROWSER: drawBrowser(); break;
    case SCR_PLAYER: drawPlayer(); break;
    case SCR_EQ: drawEq(); break;
    case SCR_SETTINGS: drawSettings(); break;
    case SCR_HELP: drawHelp(); break;
  }
}

// ============================================================ entrada
constexpr int K_ENTER = 256;
constexpr int K_TAB = 257;
constexpr int K_DEL = 258;

void stopAll() {
  stopPlayback(true);
  if (screen == SCR_PLAYER) screen = SCR_BROWSER;
  redrawCurrent();
}

bool mediaKey(int k) {
  switch (k) {
    case ' ': togglePause(); return true;
    case 'n': if (playing) advance(+1, false); return true;
    case 'p': previousOrRestart(); return true;
    case '=': case '+': changeVolume(+5); return true;
    case '-': case '_': changeVolume(-5); return true;
    case 'o': {  // alterna alto-falante / fone (cada um com seu volume)
      volByMode[outMode] = volumePercent;
      outMode = !outMode;
      volumePercent = volByMode[outMode];
      updateVolumeTarget();
      markSettingsDirty();
      char t[28];
      snprintf(t, sizeof(t), "%s %d%%", outMode ? "SAIDA: FONE" : "SAIDA: ALTO-FALANTE", volumePercent);
      showToast(t);
      redrawCurrent();
      return true;
    }
    case 'x': if (playing) stopAll(); return true;
    default: break;
  }
  if (k >= '1' && k <= '6') {
    applyPreset(k - '1');
    redrawCurrent();
    return true;
  }
  return false;
}

bool mountSD() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  const uint32_t freqs[] = {20000000, 10000000, 4000000};
  for (uint32_t f : freqs) {
    if (SD.begin(SD_CS, sdSPI, f)) return true;
    SD.end();
    delay(50);
  }
  return false;
}

void refreshBrowser() {
  if (!playing) {  // remonta o cartao apenas se nada esta tocando
    if (scanning) { if (scanHandle) scanHandle.close(); scanning = false; }
    SD.end();
    sdReady = mountSD();
  }
  char dir[DIR_SIZE];
  safeCopy(dir, sizeof(dir), browseDir);
  beginScan(dir);
}

void leaveToReturn() {
  switch (returnScreen) {
    case SCR_PLAYER: if (playing) drawPlayer(); else drawBrowser(); break;
    case SCR_EQ: drawEq(); break;
    case SCR_SETTINGS: drawSettings(); break;
    case SCR_BROWSER:
    case SCR_HELP:
    default: drawBrowser(); break;
  }
}

void activateSelection() {
  if (!entryCount || selected < 0 || selected >= (int)entryCount) return;
  if (entries[selected].isDir) {
    char name[NAME_SIZE];
    safeCopy(name, sizeof(name), entries[selected].name);
    enterDir(name);
    return;
  }
  const int chosen = buildQueueFromBrowser(selected);
  if (chosen < 0 || !queueCount) return;
  rebuildOrder(chosen);
  if (startTrack(order[orderPos], 0, true, false) || advance(+1, false)) drawPlayer();
}

void resetDefaults() {
  opt = Options();
  themeIndex = 0;
  applyTheme();
  volByMode[0] = 80; volByMode[1] = 30; outMode = 0;
  volumePercent = 80;
  updateVolumeTarget();
  applyPreset(0);
  applyBrightness();
  sleepIdx = 0;
  sleepDeadline = 0;
  applyShuffleChange();
  markSettingsDirty();
  showToast("PADROES RESTAURADOS");
  drawSettings();
}

void changeSetting(int item, int dir) {
  switch (item) {
    case 0: opt.brightness = constrain(opt.brightness + dir * 10, 10, 100); applyBrightness(); break;
    case 1: opt.screenOffIdx = (opt.screenOffIdx + dir + 4) % 4; break;
    case 2: sleepIdx = (sleepIdx + dir + 5) % 5; setSleepTimer(); break;
    case 3: if (imuOk) opt.shake = !opt.shake; break;
    case 4: opt.repeat = (opt.repeat + dir + 3) % 3; break;
    case 5: opt.shuffle = !opt.shuffle; applyShuffleChange(); break;
    case 6: opt.visual = (opt.visual + dir + 4) % 4; break;
    case 7: themeIndex = (themeIndex + dir + THEME_COUNT) % THEME_COUNT; applyTheme(); break;
    default: break;
  }
  markSettingsDirty();
  drawSettings();
}

void browserKey(int k) {
  if (k == K_TAB) { if (playing) drawPlayer(); return; }
  if (mediaKey(k)) return;
  switch (k) {
    case 's': returnScreen = SCR_BROWSER; drawSettings(); return;
    case 'e': returnScreen = SCR_BROWSER; drawEq(); return;
    case 'r': refreshBrowser(); return;
    default: break;
  }
  if (scanning) return;
  if (k == ';') moveSelection(-1, true);
  else if (k == '.') moveSelection(1, true);
  else if (k == ',') moveSelection(-ROWS, false);
  else if (k == '/') moveSelection(ROWS, false);
  else if (k == K_ENTER) activateSelection();
  else if (k == K_DEL) parentDir();
}

void playerKey(int k) {
  if (mediaKey(k)) return;
  char t[28];
  switch (k) {
    case K_TAB:
    case 'e': returnScreen = SCR_PLAYER; drawEq(); break;
    case K_DEL:
    case '`': drawBrowser(); break;
    case ',': seekBy(-10); break;
    case '/': seekBy(+10); break;
    case ';': changeVolume(+5); break;
    case '.': changeVolume(-5); break;
    case 's':
      opt.shuffle = !opt.shuffle;
      applyShuffleChange();
      markSettingsDirty();
      showToast(opt.shuffle ? "ALEATORIO: LIGADO" : "ALEATORIO: DESLIGADO");
      drawPlayer();
      break;
    case 'r':
      opt.repeat = (opt.repeat + 1) % 3;
      markSettingsDirty();
      snprintf(t, sizeof(t), "REPETIR: %s", REPEAT_NAMES[opt.repeat]);
      showToast(t);
      drawPlayer();
      break;
    case 'v':
      opt.visual = (opt.visual + 1) % 4;
      markSettingsDirty();
      snprintf(t, sizeof(t), "VISUAL: %s", VISUAL_NAMES[opt.visual]);
      showToast(t);
      break;
    case 't':
      sleepIdx = (sleepIdx + 1) % 5;
      setSleepTimer();
      if (SLEEP_MIN[sleepIdx]) snprintf(t, sizeof(t), "SONO: %d MIN", SLEEP_MIN[sleepIdx]);
      else safeCopy(t, sizeof(t), "SONO: DESLIGADO");
      showToast(t);
      break;
    default: break;
  }
}

void eqKey(int k) {
  if (mediaKey(k)) return;
  if (k == ',') { eqSelected = (eqSelected + EQ_BANDS - 1) % EQ_BANDS; drawEq(); }
  else if (k == '/') { eqSelected = (eqSelected + 1) % EQ_BANDS; drawEq(); }
  else if (k == ';' || k == '.') {
    eqGain[eqSelected] = constrain(eqGain[eqSelected] + (k == ';' ? 1 : -1), -EQ_LIMIT, EQ_LIMIT);
    configureEqualizer();
    markSettingsDirty();
    drawEq();
  } else if (k == '0') {
    applyPreset(1);  // FLAT
    drawEq();
  } else if (k == K_TAB || k == K_DEL || k == '`') {
    leaveToReturn();
  }
}

void settingsKey(int k) {
  if (mediaKey(k)) return;
  if (k == ';') { settingsSel = (settingsSel + SETTINGS_COUNT - 1) % SETTINGS_COUNT; drawSettings(); }
  else if (k == '.') { settingsSel = (settingsSel + 1) % SETTINGS_COUNT; drawSettings(); }
  else if (k == ',') changeSetting(settingsSel, -1);
  else if (k == '/') changeSetting(settingsSel, +1);
  else if (k == K_ENTER) {
    if (settingsSel == SETTINGS_COUNT - 1) resetDefaults();
    else changeSetting(settingsSel, +1);
  } else if (k == K_TAB || k == K_DEL || k == '`') {
    leaveToReturn();
  }
}

void onKey(int k) {
  if (k >= 'A' && k <= 'Z') k += 32;

  // H abre a ajuda global; na ajuda, H percorre as tres paginas.
  if (screen == SCR_HELP) {
    if (k == 'h') {
      helpPage = (helpPage + 1) % 3;
      drawHelp();
    } else if (k == K_TAB || k == K_DEL || k == '`') {
      leaveToReturn();
    }
    return;
  }
  if (k == 'h') {
    returnScreen = screen;
    helpPage = 0;
    drawHelp();
    return;
  }

  switch (screen) {
    case SCR_BROWSER: browserKey(k); break;
    case SCR_PLAYER: playerKey(k); break;
    case SCR_EQ: eqKey(k); break;
    case SCR_SETTINGS: settingsKey(k); break;
    case SCR_HELP: leaveToReturn(); break;
  }
}

bool isRepeatable(int k) {
  if (k >= 'A' && k <= 'Z') k += 32;
  if (k == '-' || k == '=' || k == '_' || k == '+') return true;
  if (screen == SCR_BROWSER) return k == ';' || k == '.' || k == ',' || k == '/';
  if (screen == SCR_PLAYER || screen == SCR_EQ) return k == ';' || k == '.';
  return false;
}

// Retorna true se a tela estava apagada (a tecla so acorda a tela).
bool wakeIfScreenOff() {
  lastActivity = millis();
  if (!screenOff) return false;
  screenOff = false;
  applyBrightness();
  redrawCurrent();
  return true;
}

int heldKey = 0;
uint32_t nextRepeat = 0;

void handleKeyboard() {
  if (M5Cardputer.Keyboard.isChange()) {
    if (M5Cardputer.Keyboard.isPressed()) {
      auto ks = M5Cardputer.Keyboard.keysState();
      const bool woke = wakeIfScreenOff();
      heldKey = 0;
      if (!woke) {
        if (ks.tab) onKey(K_TAB);
        if (ks.enter) onKey(K_ENTER);
        if (ks.del) onKey(K_DEL);
        if (ks.space) onKey(' ');
        for (char c : ks.word) onKey((unsigned char)c);
        if (ks.word.size() == 1 && !ks.tab && !ks.enter && !ks.del && !ks.space &&
            isRepeatable((unsigned char)ks.word[0])) {
          heldKey = (unsigned char)ks.word[0];
          nextRepeat = millis() + 380;
        }
      }
    } else {
      heldKey = 0;
    }
  }
  if (heldKey) {
    if (!M5Cardputer.Keyboard.isPressed()) {
      heldKey = 0;
    } else if ((int32_t)(millis() - nextRepeat) >= 0) {
      nextRepeat = millis() + 90;
      lastActivity = millis();
      onKey(heldKey);
    }
  }
}

void pollShake() {
  static uint32_t lastPoll = 0, lastShake = 0;
  if (!imuOk || !opt.shake || !playing) return;
  const uint32_t now = millis();
  if (now - lastPoll < 20) return;
  lastPoll = now;
  M5.Imu.update();
  auto d = M5.Imu.getImuData();
  const float m = sqrtf(d.accel.x * d.accel.x + d.accel.y * d.accel.y + d.accel.z * d.accel.z);
  if (m > SHAKE_G && now - lastShake > 1500) {
    lastShake = now;
    advance(+1, false);
  }
}

void decodeStep() {
  if (!playing || paused || !decoder) return;
  bool ended = false;
  const uint32_t t0 = millis();
  while (millis() - t0 < 8) {  // fatia curta: UI e teclado nunca ficam presos
    gSliceLeft = 2304;           // ~2 frames por chamada
    if (!decoder->loop()) { ended = true; break; }
    if (audioOutput && audioOutput->isFull()) break;
  }
  if (ended) { onTrackEnded(); return; }
  if (audioOutput && audioOutput->isFull()) delay(2);  // fila do speaker cheia

  // anti-travamento: sem progresso por 4 s = arquivo ruim/SD com problema
  if (gSamplesSubmitted != lastSamples) {
    lastSamples = gSamplesSubmitted;
    lastProgress = millis();
  } else if (millis() - lastProgress > 4000) {
    lastProgress = millis();
    showToast("ERRO DE LEITURA");
    advance(+1, true);
  }
}

void fadeStep() {
  if (fadeAction == FA_NONE || !playing) return;
  if (gFadeGain > 0.001f) return;
  if (fadeAction == FA_PAUSE) {
    paused = true;
    fadeAction = FA_NONE;
    memset(gBand, 0, sizeof(gBand));
    memset(gScope, 0, sizeof(gScope));
    redrawCurrent();
  } else {
    stopPlayback(true);
    if (screen == SCR_PLAYER) screen = SCR_BROWSER;
    redrawCurrent();
  }
}

// RGB LED do ADV (GPIO21): segue o tema e reage ao audio.
void ledUpdate() {
  static uint32_t last = 0;
  static int lastR = -1, lastG = -1, lastB = -1;
  static float level = 0.10f;
  const uint32_t now = millis();
  if (now - last < 40) return;
  last = now;
  const ThemePalette &theme = THEMES[themeIndex];
  const float audio = (playing && !paused)
      ? (gBand[0] + gBand[1] + gBand[2]) / 3.0f : 0.0f;
  const float target = (playing && !paused)
      ? constrain(0.10f + audio * 1.15f, 0.10f, 1.0f)
      : (paused ? 0.055f : 0.12f);
  level = target > level ? target : level * 0.78f;
  const int r = (int)(theme.r * level);
  const int g = (int)(theme.g * level);
  const int b = (int)(theme.b * level);
  if (r != lastR || g != lastG || b != lastB) {
    lastR = r; lastG = g; lastB = b;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    neopixelWrite(21, (uint8_t)r, (uint8_t)g, (uint8_t)b);
#endif
  }
}

void housekeeping() {
  const uint32_t now = millis();
  static uint32_t lastLive = 0, lastBrowser = 0, lastScanDraw = 0;

  if (!screenOff) {
    if (screen == SCR_PLAYER && playing && now - lastLive >= 50) {
      lastLive = now;
      drawLive();
    }
    if (screen == SCR_BROWSER) {
      if (scanning) {
        if (now - lastScanDraw >= 250) { lastScanDraw = now; drawBrowser(); }
      } else if (playing && now - lastBrowser >= 1000) {
        lastBrowser = now;
        drawBrowser();
      }
    }
  }

  if (toastActive && (int32_t)(now - toastUntil) > 0) {
    toastActive = false;
    if (screen == SCR_BROWSER) drawBrowser();
  }

  if (sleepDeadline && (int32_t)(now - sleepDeadline) >= 0) {
    sleepDeadline = 0;
    sleepIdx = 0;
    if (playing) {
      if (paused) {
        stopPlayback(true);
        if (screen == SCR_PLAYER) screen = SCR_BROWSER;
        redrawCurrent();
      } else {
        fadeAction = FA_STOP;
        gFadeTarget = 0.0f;
      }
    }
  }

  const int offSec = SCREEN_OFF_SEC[opt.screenOffIdx];
  if (offSec && !screenOff && now - lastActivity > (uint32_t)offSec * 1000UL) {
    screenOff = true;
    M5Cardputer.Display.setBrightness(0);
  }

  if (now - lastBatteryRead > 15000) {
    lastBatteryRead = now;
    batteryPercent = M5Cardputer.Power.getBatteryLevel();
  }

  if (settingsDirty && now - settingsChangedAt > 2000) saveSettings();
}

}  // namespace

// ================================================================ setup / loop
void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  loadSettings();
  applyBrightness();
  drawSplash();
  const uint32_t splashStart = millis();

  ui.setColorDepth(16);
  if (!ui.createSprite(240, 135)) Serial.println("ERRO: sprite UI sem memoria");
  live.setColorDepth(16);
  if (!live.createSprite(240, 90)) Serial.println("ERRO: sprite live sem memoria");
  coverCanvas.setColorDepth(16);
  coverCanvasReady = coverCanvas.createSprite(68, 46);
  if (!coverCanvasReady) Serial.println("AVISO: capa indisponivel (sem memoria)");

  // Speaker: 44.1 kHz nativo, mais DMA, tarefa de audio no core 0
  M5Cardputer.Speaker.end();
  auto spk = M5Cardputer.Speaker.config();
  spk.sample_rate = 44100;
  spk.dma_buf_count = 6;
  spk.dma_buf_len = 512;
  spk.task_pinned_core = 0;
  M5Cardputer.Speaker.config(spk);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(220);  // ganho moderado; volume final controlado no player

  updateVolumeTarget();
  gVolCur = gVolTarget;
  configureEqualizer();
  configureSpectrum();

  imuOk = M5.Imu.isEnabled();
  batteryPercent = M5Cardputer.Power.getBatteryLevel();
  lastActivity = millis();

  sdReady = mountSD();
  screen = SCR_BROWSER;
  while (millis() - splashStart < 900) delay(10);
  beginScan((sdReady && SD.exists("/music")) ? "/music" : "/");

  Serial.printf("MP3 PLAYER %s | SD=%s | IMU=%s | heap=%u\n", MP3_PLAYER_VERSION,
                sdReady ? "OK" : "FAIL", imuOk ? "OK" : "N/D",
                (unsigned)ESP.getFreeHeap());
}

void loop() {
  M5Cardputer.update();
  decodeStep();
  fadeStep();
  scanStep(5);
  handleKeyboard();
  pollShake();
  housekeeping();
  ledUpdate();
  delay(1);
}
