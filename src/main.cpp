/*
 * MP3 PLAYER - player de audio para M5Stack Cardputer ADV
 * Versao 6.0
 *
 * Base consolidada v7.0 (v6.0 + compatibilidade/fluxo da v5.0)\n * Playback modes: 7=SLOWED (0.75x), 8=SPED UP (1.25x), 9=Nightcore (1.75x), 0=NORMAL.\n * NOVIDADES DA v6.0
 *  SOM
 *   - Saida fixa em 44.1 kHz com reamostrador cubico (Catmull-Rom) proprio:
 *     arquivos 48k/32k/22k tocam limpos e na velocidade certa.
 *   - Cadeia de audio nova: HPF do alto-falante, EQ 5 bandas com preamp
 *     calculado pela resposta real, contorno de loudness (graves/agudos
 *     sobem quando o volume e baixo), compressor suave, graves virtuais
 *     (harmonicas que o alto-falante pequeno consegue reproduzir), limitador
 *     suave e dither TPDF. Ajuste "REALCE DE SOM" (tecla G): desligado/suave/forte.
 *   - Fila do speaker corrigida (nao corta mais o bloco que esta tocando,
 *     que era a causa da musica "acelerando"/pulando). Mais DMA e blocos
 *     maiores para tolerar picos de leitura do SD.
 *  FORMATOS
 *   - MP3, WAV (PCM), FLAC e AAC (ADTS .aac). FLAC/AAC/WAV so entram se as
 *     classes existirem na sua versao do ESP8266Audio (__has_include).
 *   - M4A/OGG/OPUS nao sao suportados pela biblioteca.
 *  CAPAS
 *   - Sem limite de 24 KB: a capa e decodificada em streaming direto do
 *     arquivo, ja reduzida para 44x44. ID3v2.2/2.3/2.4 (APIC/PIC), FLAC
 *     PICTURE e arquivos cover/folder/front/album .jpg/.png na pasta.
 *     JPEG progressivo nao e suportado pelo decodificador (cai para a pasta).
 *   - O audio continua sendo decodificado durante a leitura da capa.
 *  SD / TRAVAMENTOS
 *   - Fonte de arquivo propria com recuperacao: se o SD falhar durante a
 *     musica, remonta o cartao, reabre o arquivo e continua do mesmo ponto.
 *   - Buffer de leitura de 48 KB na RAM interna, SD a 12 MHz (mais estavel),
 *     ate 12 arquivos abertos (o padrao de 5 estourava com a busca de pastas).
 *   - Busca +-10 s e proxima/anterior agrupadas (debounce): varios toques
 *     rapidos viram uma unica troca. Busca MP3 usa tabela TOC do Xing (VBR
 *     preciso) e alinha no inicio de um frame valido.
 *   - Cabecalho Xing/Info/VBRI e pulado (menos silencio entre faixas).
 *
 * Dependencias: M5Cardputer (M5Unified/M5GFX) e ESP8266Audio (earlephilhower)
 * Pasta do sketch: MP3_Player/MP3_Player.ino
 *
 * TECLAS (H abre o tutorial em qualquer tela)
 *  LISTA   ; . sobe/desce   , / pagina   ENTER abre/toca   DEL pasta acima
 *          M troca SD/FLASH  C copia SD->FLASH   TAB player   S ajustes   E equalizador   R atualiza
 *          ESPACO pausa  N P faixa  - = volume  1-6 preset  G realce  X para
 *  PLAYER  ESPACO pausa  N proxima  P anterior/reinicia  , / busca 10 s
 *          ; . volume    - = volume  1-6 preset  TAB equalizador  G realce
 *          S aleatorio   R repetir   V visual    T timer de sono
 *          7 SLOWED  8 SPED UP  9 Nightcore  0 NORMAL
 *          X para        DEL/` volta a lista (musica continua)
 *  EQ      , / banda     ; . ganho   0 zera      TAB/DEL/` volta
 */

#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <math.h>
#include <new>

#include <AudioOutput.h>
#include <AudioFileSource.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGenerator.h>
#include <AudioGeneratorMP3.h>
#if __has_include(<AudioGeneratorWAV.h>)
#include <AudioGeneratorWAV.h>
#define HAVE_WAV 1
#endif
#if __has_include(<AudioGeneratorFLAC.h>)
#include <AudioGeneratorFLAC.h>
#define HAVE_FLAC 1
#endif
#if __has_include(<AudioGeneratorAAC.h>)
#include <AudioGeneratorAAC.h>
#define HAVE_AAC 1
#endif

#define MP3_PLAYER_VERSION "8.1.0"

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
uint16_t C_BG = rgb(4, 8, 15);
uint16_t C_RED = rgb(77, 163, 255);
uint16_t C_DARKRED = rgb(8, 28, 52);
uint16_t C_PANEL = rgb(10, 27, 46);
uint16_t C_PANEL2 = rgb(5, 15, 27);
uint16_t C_DIM = rgb(14, 35, 57);
constexpr uint16_t C_WHITE   = rgb(255, 255, 255);
constexpr uint16_t C_GREY    = rgb(150, 150, 160);
uint16_t C_CYAN = rgb(0, 229, 255);
constexpr uint16_t C_BLUE    = rgb(70, 120, 255);
constexpr uint16_t C_MAGENTA = rgb(255, 70, 190);
constexpr uint16_t C_ORANGE  = rgb(255, 140, 0);
constexpr uint16_t C_YELLOW  = rgb(255, 230, 60);

// ============================================================ constantes
constexpr uint32_t OUT_RATE = 44100;               // taxa fixa da saida
constexpr size_t SD_AUDIO_BUFFER_SIZE = 48 * 1024; // readahead na RAM interna
constexpr size_t BLOCK_SAMPLES = 2048;             // amostras por bloco no speaker
constexpr size_t RING_BLOCKS   = 4;                // speaker segura no maximo 2
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
constexpr int    COVER_PX      = 44;
constexpr uint32_t COVER_MAX_BYTES = 900UL * 1024UL;

static uint8_t sdAudioBuffer[SD_AUDIO_BUFFER_SIZE];

// ============================================================ estado geral
enum Screen { SCR_BROWSER, SCR_PLAYER, SCR_EQ, SCR_SETTINGS, SCR_HELP };
Screen screen = SCR_BROWSER;
Screen returnScreen = SCR_BROWSER;
int helpPage = 0;

enum FadeAction { FA_NONE, FA_PAUSE, FA_STOP };
FadeAction fadeAction = FA_NONE;

enum Fmt : uint8_t { FMT_MP3, FMT_WAV, FMT_FLAC, FMT_AAC, FMT_NONE };
const char *FMT_NAMES[5] = {"MP3", "WAV", "FLAC", "AAC", "?"};

bool sdReady = false;
bool internalReady = false;
enum StorageKind : uint8_t { STORAGE_SD, STORAGE_INTERNAL };
StorageKind storageKind = STORAGE_SD;
fs::FS &activeFS() { return storageKind == STORAGE_SD ? (fs::FS &)SD : (fs::FS &)LittleFS; }
bool storageReady() { return storageKind == STORAGE_SD ? sdReady : internalReady; }
bool playing = false;   // decoder ativo (inclui pausado)
bool paused = false;
bool imuOk = false;

int volumePercent = 80;
int volByMode[2] = {80, 30};  // 0 alto-falante, 1 fone
int outMode = 0;
int eqGain[EQ_BANDS] = {3, 1, 0, 2, 3};
int eqSelected = 0;
int settingsSel = 0;
int uiLanguage = 0;  // 0 English, 1 Portuguese, 2 Spanish, 3 Chinese, 4 Japanese, 5 French, 6 German, 7 Russian, 8 Italian
int sleepIdx = 0;
uint32_t sleepDeadline = 0;

struct Options {
  int brightness = 70;
  int screenOffIdx = 0;
  int shake = 0;
  int repeat = 0;   // 0 off, 1 todas, 2 uma
  int shuffle = 0;
  int visual = 0;   // 0 barras, 1 espelho, 2 onda, 3 capa
  int enhance = 1;  // 0 desligado, 1 suave, 2 forte
} opt;

const int SCREEN_OFF_SEC[4] = {0, 15, 30, 60};
const int SLEEP_MIN[5] = {0, 15, 30, 60, 90};
const char *REPEAT_NAMES[3] = {"OFF", "ALL", "ONE"};
const char *VISUAL_NAMES[4] = {"BARS", "MIRROR", "WAVE", "COVER"};
const char *ENHANCE_NAMES[3] = {"OFF", "SOFT", "STRONG"};

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
int themeIndex = 3;

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
void sdRemount();
void pumpAudio();

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

void stripExt(char *s) {
  char *d = strrchr(s, '.');
  if (d && d != s && strlen(d) <= 5) *d = '\0';
}

Fmt formatOf(const char *name) {
  const char *dot = name ? strrchr(name, '.') : nullptr;
  if (!dot) return FMT_NONE;
  if (!strcasecmp(dot, ".mp3")) return FMT_MP3;
#ifdef HAVE_WAV
  if (!strcasecmp(dot, ".wav")) return FMT_WAV;
#endif
#ifdef HAVE_FLAC
  if (!strcasecmp(dot, ".flac")) return FMT_FLAC;
#endif
#ifdef HAVE_AAC
  if (!strcasecmp(dot, ".aac")) return FMT_AAC;
#endif
  return FMT_NONE;
}
bool isAudioFile(const char *name) { return formatOf(name) != FMT_NONE; }

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

  // type: 0 peaking, 1 low shelf, 2 high shelf, 3 high-pass (RBJ)
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
    } else if (type == 3) {
      nb0 = (1 + cw) * 0.5f; nb1 = -(1 + cw); nb2 = (1 + cw) * 0.5f;
      na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha;
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
    active = (type == 3) || fabsf(gainDb) > 0.05f;
  }

  float magDb(float f, float fs) const {
    const float w = 2.0f * PI * f / fs;
    const float c1 = cosf(w), s1 = sinf(w), c2 = cosf(2 * w), s2 = sinf(2 * w);
    const float nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    const float dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return 10.0f * log10f((nr * nr + ni * ni) / (dr * dr + di * di + 1e-12f) + 1e-12f);
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
Biquad loudLow, loudHigh, spkHpf;

float gSrcRate = 44100.0f;   // taxa original do arquivo (so exibicao)
float gPreamp = 1.0f;
float gVolTarget = 0.5f;
float gVolCur = 0.5f;
float gFadeGain = 0.0f;
float gFadeTarget = 1.0f;
float gFadeStep = 1.0f / (0.03f * 44100.0f);
uint32_t gSamplesSubmitted = 0;

// realce de som
bool gCompOn = false;
float gCompEnv = 0, gCompThr = 12000.0f, gCompMake = 1.1f;
float gVBassMix = 0, gBassLp = 0, gBassHp = 0;
uint32_t gRng = 0x1234ABCDu;

// reamostrador (arquivo -> 44.1 kHz)
bool rsActive = false;
float rsStep = 1.0f, rsPhase = 0.0f;
float rsH[4] = {0, 0, 0, 0};
float playbackSpeed = 1.0f;  // SLOWED 0.75x, NORMAL 1.00x, SPED UP 1.25x, Nightcore 1.75x

float gBand[SPEC_BANDS] = {0};
int16_t gScope[128] = {0};
const float SPEC_FREQS[SPEC_BANDS] = {60, 100, 170, 300, 500, 800, 1300, 2000, 3200, 5000, 8000, 12000};
float gGoertzel[SPEC_BANDS];
bool gGoertzelOk[SPEC_BANDS];

// O speaker guarda PONTEIROS: os blocos precisam sobreviver a troca de faixa.
int16_t gRing[RING_BLOCKS][BLOCK_SAMPLES];
size_t gCur = 0;
size_t gFill = 0;
int gSliceLeft = 0;

inline float fastTanh(float x) {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

void configureDsp() {
  const float fs = (float)OUT_RATE;
  const bool enh = opt.enhance > 0;
  const bool spk = (outMode == 0);
  for (int i = 0; i < EQ_BANDS; ++i) {
    const int type = (i == 0) ? 1 : (i == EQ_BANDS - 1 ? 2 : 0);
    const float q = (type == 0) ? 1.0f : 0.707f;
    eq[i].configure(type, fs, EQ_FREQS[i], (float)eqGain[i], q);
  }
  const float v = (float)volumePercent / 100.0f;
  const float lc = enh ? (opt.enhance == 2 ? 1.0f : 0.6f) : 0.0f;
  loudLow.configure(1, fs, 110.0f, (1.0f - v) * 5.0f * lc, 0.6f);
  loudHigh.configure(2, fs, 8000.0f, (1.0f - v) * 2.5f * lc, 0.6f);
  if (enh && spk) spkHpf.configure(3, fs, opt.enhance == 2 ? 105.0f : 85.0f, 0, 0.707f);
  else spkHpf.active = false;
  gVBassMix = (enh && spk) ? (opt.enhance == 2 ? 0.45f : 0.25f) : 0.0f;
  gCompOn = enh;
  gCompThr = (opt.enhance == 2) ? 9000.0f : 12000.0f;
  gCompMake = (opt.enhance == 2) ? 1.30f : 1.12f;

  // headroom: preamp compensa o pico real da curva (EQ + loudness)
  float peak = 0;
  for (int k = 0; k < 48; ++k) {
    const float f = 30.0f * powf(600.0f, (float)k / 47.0f);
    float db = 0;
    for (int i = 0; i < EQ_BANDS; ++i) if (eq[i].active) db += eq[i].magDb(f, fs);
    if (loudLow.active) db += loudLow.magDb(f, fs);
    if (loudHigh.active) db += loudHigh.magDb(f, fs);
    if (db > peak) peak = db;
  }
  gPreamp = (peak > 0.1f) ? powf(10.0f, -(peak + 0.3f) / 20.0f) : 1.0f;
}

void resetDsp() {
  for (int i = 0; i < EQ_BANDS; ++i) eq[i].reset();
  loudLow.reset(); loudHigh.reset(); spkHpf.reset();
  gCompEnv = 0; gBassLp = 0; gBassHp = 0;
}

void configureSpectrum() {
  for (int i = 0; i < SPEC_BANDS; ++i) {
    if (SPEC_FREQS[i] < OUT_RATE * 0.45f) {
      gGoertzel[i] = 2.0f * cosf(2.0f * PI * SPEC_FREQS[i] / (float)OUT_RATE);
      gGoertzelOk[i] = true;
    } else {
      gGoertzel[i] = 0;
      gGoertzelOk[i] = false;
    }
  }
}

void updateVolumeTarget() {
  const float v = (float)volumePercent / 100.0f;
  gVolTarget = v * v * (outMode == 0 ? 1.6f : 1.0f);
  configureDsp();
}

void analyzeBlock(const int16_t *b, size_t n) {
  if (!n) return;
  const float norm = 1.0f / fmaxf(gVolCur, 0.2f);
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
    const float amp = sqrtf(p) * 2.0f / (float)n * norm;
    const float db = 20.0f * log10f(amp / 32768.0f + 1e-6f) + (float)i * 0.9f;
    float lvl = (db + 54.0f) / 54.0f;
    if (lvl < 0) lvl = 0;
    if (lvl > 1) lvl = 1;
    gBand[i] = lvl;
  }
}

bool submitBlock() {
  if (!gFill) return true;
  // Fila do speaker: no maximo 2 blocos pendentes. NUNCA interrompe o que toca.
  if (M5Cardputer.Speaker.isPlaying(0) >= 2) return false;
  if (!M5Cardputer.Speaker.playRaw(gRing[gCur], gFill, OUT_RATE, false, 1, 0, false))
    return false;
  analyzeBlock(gRing[gCur], gFill);
  if (gFill >= 256) {
    for (int k = 0; k < 128; ++k) gScope[k] = gRing[gCur][(size_t)k * gFill / 128];
  }
  gSamplesSubmitted += (uint32_t)gFill;
  gCur = (gCur + 1) % RING_BLOCKS;
  gFill = 0;
  return true;
}

inline float rnd01() {
  gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5;
  return (float)(gRng & 0xFFFF) * (1.0f / 65536.0f);
}

// Cadeia completa para UMA amostra mono a 44.1 kHz.
inline int16_t processSample(float x) {
  if (spkHpf.active) x = spkHpf.process(x);
  for (int i = 0; i < EQ_BANDS; ++i) if (eq[i].active) x = eq[i].process(x);
  if (loudLow.active) x = loudLow.process(x);
  if (loudHigh.active) x = loudHigh.process(x);
  x *= gPreamp;

  if (gCompOn) {  // compressor suave 2:1 com make-up
    const float a = fabsf(x);
    gCompEnv += (a > gCompEnv ? 0.0113f : 0.00015f) * (a - gCompEnv);
    float g = gCompMake;
    if (gCompEnv > gCompThr) g *= sqrtf(gCompThr / gCompEnv);
    x *= g;
  }
  if (gVBassMix > 0.0f) {  // graves virtuais: harmonicas dos graves
    gBassLp += 0.0185f * (x - gBassLp);
    const float h = fastTanh(gBassLp * (1.0f / 3000.0f)) * 3000.0f;
    gBassHp += 0.0215f * (h - gBassHp);
    x += gVBassMix * (h - gBassHp);
  }

  gVolCur += (gVolTarget - gVolCur) * 0.0015f;
  if (gFadeGain < gFadeTarget) {
    gFadeGain += gFadeStep;
    if (gFadeGain > gFadeTarget) gFadeGain = gFadeTarget;
  } else if (gFadeGain > gFadeTarget) {
    gFadeGain -= gFadeStep;
    if (gFadeGain < gFadeTarget) gFadeGain = gFadeTarget;
  }
  x *= gVolCur * gFadeGain;

  const float a2 = fabsf(x);  // limitador suave
  if (a2 > 22000.0f) x = copysignf(22000.0f + 10000.0f * fastTanh((a2 - 22000.0f) / 10000.0f), x);

  if (gFadeGain > 0.001f) x += rnd01() - rnd01();  // dither TPDF
  int v = (int)lrintf(x);
  if (v > 32767) v = 32767;
  if (v < -32767) v = -32767;
  return (int16_t)v;
}

inline float cubic4(const float *h, float t) {
  const float a = -0.5f * h[0] + 1.5f * h[1] - 1.5f * h[2] + 0.5f * h[3];
  const float b = h[0] - 2.5f * h[1] + 2.0f * h[2] - 0.5f * h[3];
  const float c = -0.5f * h[0] + 0.5f * h[2];
  return ((a * t + b) * t + c) * t + h[1];
}

class AudioOut final : public AudioOutput {
public:
  bool begin() override {
    for (int i = 0; i < 4; ++i) rsH[i] = 0;
    rsPhase = 0;
    applyRate();
    return true;
  }

  bool SetRate(int hz) override {
    AudioOutput::SetRate(hz);
    applyRate();
    return true;
  }

  // true = bloco quase cheio e o speaker ainda nao aceitou
  bool isFull() const {
    if (gFill + 8 <= BLOCK_SAMPLES) return false;
    return !submitBlock();
  }

  bool ConsumeSample(int16_t sample[2]) override {
    if (gSliceLeft <= 0) return false;  // fatia esgotada: devolve o controle ao loop()
    if (gFill + 8 > BLOCK_SAMPLES && !submitBlock()) return false;  // contrapressao
    --gSliceLeft;
    const float x = ((float)sample[0] + (float)sample[1]) * 0.5f;  // speaker mono
    if (!rsActive) {
      gRing[gCur][gFill++] = processSample(x);
    } else {
      rsH[0] = rsH[1]; rsH[1] = rsH[2]; rsH[2] = rsH[3]; rsH[3] = x;
      while (rsPhase < 1.0f && gFill < BLOCK_SAMPLES) {
        gRing[gCur][gFill++] = processSample(cubic4(rsH, rsPhase));
        rsPhase += rsStep;
      }
      rsPhase -= 1.0f;
      if (rsPhase < 0) rsPhase = 0;
    }
    if (gFill + 8 > BLOCK_SAMPLES) submitBlock();
    return true;
  }

  void flush() override {
    const uint32_t t = millis();
    while (gFill && !submitBlock() && millis() - t < 150) delay(2);
  }

  bool stop() override {
    flush();
    return true;
  }

private:
  void applyRate() {
    const int hz = (hertz >= 8000 && hertz <= 192000) ? hertz : 44100;
    gSrcRate = (float)hz;
    rsActive = (hz != (int)OUT_RATE) || fabsf(playbackSpeed - 1.0f) > 0.001f;
    rsStep = ((float)hz / (float)OUT_RATE) * playbackSpeed;
  }
};

// ============================================================ SD resiliente
bool mountSD();

// Fonte de arquivo com recuperacao: se o SD falhar no meio da musica,
// remonta o cartao, reabre o arquivo e retoma exatamente do mesmo byte.
class SdStreamSource final : public AudioFileSource {
public:
  bool open(const char *path) override {
    close();
    safeCopy(path_, sizeof(path_), path);
    f_ = activeFS().open(path_, FILE_READ);
    if (!f_) return false;
    size_ = (uint32_t)f_.size();
    pos_ = 0;
    return true;
  }

  uint32_t read(void *data, uint32_t len) override {
    if (!f_ || !len) return 0;
    uint8_t *d = (uint8_t *)data;
    uint32_t got = 0;
    int fails = 0;
    while (got < len) {
      const size_t r = f_.read(d + got, len - got);
      if (r > 0) { got += (uint32_t)r; pos_ += (uint32_t)r; fails = 0; continue; }
      if (pos_ >= size_) break;               // fim real do arquivo
      if (++fails > 3 || !recover()) break;   // erro de SD: tenta recuperar
    }
    return got;
  }

  bool seek(int32_t pos, int dir) override {
    int64_t t = pos;
    if (dir == SEEK_CUR) t = (int64_t)pos_ + pos;
    else if (dir == SEEK_END) t = (int64_t)size_ + pos;
    if (t < 0 || t > (int64_t)size_) return false;
    pos_ = (uint32_t)t;
    if (f_ && f_.seek(pos_)) return true;
    return recover();
  }

  bool close() override { if (f_) f_.close(); return true; }
  bool isOpen() override { return (bool)f_; }
  uint32_t getSize() override { return size_; }
  uint32_t getPos() override { return pos_; }

private:
  bool recover() {
    if (f_) f_.close();
    if (storageKind != STORAGE_SD) return false;
    sdRemount();
    f_ = SD.open(path_, FILE_READ);
    if (!f_) return false;
    return f_.seek(pos_);
  }

  File f_;
  char path_[PATH_SIZE] = {0};
  uint32_t size_ = 0;
  uint32_t pos_ = 0;
};

SdStreamSource *source = nullptr;
AudioFileSourceBuffer *bufferedSource = nullptr;
AudioGenerator *decoder = nullptr;
AudioOut *audioOutput = nullptr;

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
  uiLanguage = constrain(prefs.getInt("lang", 0), 0, 8);
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
  opt.enhance = constrain(prefs.getInt("en", 1), 0, 2);
  const int themeVersion = prefs.getInt("themeV", 0);
  themeIndex = constrain(prefs.getInt("th", 3), 0, THEME_COUNT - 1);
  if (themeVersion < 1) themeIndex = 3;
  for (int i = 0; i < EQ_BANDS; ++i) {
    char key[3] = {'g', (char)('0' + i), 0};
    eqGain[i] = constrain(prefs.getInt(key, eqGain[i]), -EQ_LIMIT, EQ_LIMIT);
  }
  prefs.end();
  if (themeVersion < 1) {
    prefs.begin("mp3player", false);
    prefs.putInt("th", 3);
    prefs.putInt("themeV", 1);
    prefs.end();
  }
  applyTheme();
}

void saveSettings() {
  prefs.begin("mp3player", false);
  volByMode[outMode] = volumePercent;
  prefs.putInt("lang", uiLanguage);
  prefs.putInt("vs", volByMode[0]);
  prefs.putInt("vp", volByMode[1]);
  prefs.putInt("om", outMode);
  prefs.putInt("br", opt.brightness);
  prefs.putInt("so", opt.screenOffIdx);
  prefs.putInt("sh", opt.shake);
  prefs.putInt("rp", opt.repeat);
  prefs.putInt("sf", opt.shuffle);
  prefs.putInt("vz", opt.visual);
  prefs.putInt("en", opt.enhance);
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
  configureDsp();
  markSettingsDirty();
}

// ============================================================ metadados
struct Meta {
  char title[64];
  char artist[40];
  uint32_t durationSec;
  uint32_t audioStart;   // onde o decoder comeca (apos tags e cabecalho Xing)
  uint32_t seekBase;     // base para calculo de busca (primeiro frame)
  uint32_t fileSize;
  uint8_t toc[100];
  bool hasToc;
  Fmt fmt;
};
Meta meta;
char currentTitle[110] = {0};
char currentPath[PATH_SIZE] = {0};

struct CoverRef {
  bool valid = false;
  bool png = false;
  uint16_t w = 0, h = 0;
  uint32_t off = 0, len = 0;
  char path[PATH_SIZE] = {0};
};
CoverRef coverRef;
bool coverCanvasReady = false;
bool coverLoaded = false;
bool coverPending = false;
char gProbePath[PATH_SIZE] = {0};

static uint8_t gProbeBuf[2048];
static uint8_t gTextBuf[200];

uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint32_t le32(const uint8_t *p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}
uint32_t syncsafe(const uint8_t *p) {
  return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
         ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

// ---- MP3: cabecalhos de frame
struct Mp3Hdr { uint32_t bitrate, sr, spf, frameLen; bool mpeg1, mono; };
const uint16_t BR1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
const uint16_t BR2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
const uint32_t SR1[4] = {44100, 48000, 32000, 0};
const uint32_t SR2[4] = {22050, 24000, 16000, 0};
const uint32_t SR25[4] = {11025, 12000, 8000, 0};

bool parseMp3Hdr(const uint8_t *p, Mp3Hdr &h) {
  if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
  const int verBits = (p[1] >> 3) & 3, layerBits = (p[1] >> 1) & 3;
  const int brIdx = (p[2] >> 4) & 0xF, srIdx = (p[2] >> 2) & 3;
  if (verBits == 1 || layerBits != 1 || brIdx == 0 || brIdx == 15 || srIdx == 3) return false;
  h.mpeg1 = (verBits == 3);
  h.bitrate = h.mpeg1 ? BR1[brIdx] : BR2[brIdx];
  h.sr = h.mpeg1 ? SR1[srIdx] : (verBits == 2 ? SR2[srIdx] : SR25[srIdx]);
  h.spf = h.mpeg1 ? 1152 : 576;
  h.mono = ((p[3] >> 6) & 3) == 3;
  const uint32_t pad = (p[2] >> 1) & 1;
  h.frameLen = (h.mpeg1 ? 144UL : 72UL) * h.bitrate * 1000UL / h.sr + pad;
  return h.frameLen >= 24;
}

// Acha um frame valido confirmado pelo frame seguinte.
int findMp3Sync(const uint8_t *b, size_t got) {
  for (size_t i = 0; i + 4 <= got; ++i) {
    Mp3Hdr h;
    if (!parseMp3Hdr(b + i, h)) continue;
    const size_t nx = i + h.frameLen;
    if (nx + 4 <= got) {
      Mp3Hdr h2;
      if (!parseMp3Hdr(b + nx, h2) || h2.sr != h.sr) continue;
    }
    return (int)i;
  }
  return -1;
}

uint32_t alignMp3(const char *path, uint32_t pos) {
  File f = activeFS().open(path, FILE_READ);
  if (!f) return pos;
  uint32_t r = pos;
  if (f.seek(pos)) {
    const size_t got = f.read(gProbeBuf, sizeof(gProbeBuf));
    const int i = findMp3Sync(gProbeBuf, got);
    if (i >= 0) r = pos + (uint32_t)i;
  }
  f.close();
  return r;
}

// ---- imagens
bool jpegInfo(File &f, uint32_t off, uint32_t len, uint16_t &w, uint16_t &h, bool &prog) {
  uint32_t p = off + 2;
  const uint32_t end = off + len;
  uint8_t b[9];
  for (int g = 0; g < 48 && p + 4 < end; ++g) {
    if (!f.seek(p) || f.read(b, 4) != 4) return false;
    if (b[0] != 0xFF) return false;
    const uint8_t m = b[1];
    if (m == 0xFF) { ++p; continue; }
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { p += 2; continue; }
    const uint32_t seg = ((uint32_t)b[2] << 8) | b[3];
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (f.read(b, 5) != 5) return false;
      h = (uint16_t)((b[1] << 8) | b[2]);
      w = (uint16_t)((b[3] << 8) | b[4]);
      prog = (m == 0xC2);
      return w > 0 && h > 0;
    }
    p += 2 + seg;
  }
  return false;
}

bool inspectImage(File &f, uint32_t off, uint32_t len, CoverRef &c) {
  if (len < 32 || len > COVER_MAX_BYTES) return false;
  uint8_t b[24];
  if (!f.seek(off) || f.read(b, 24) != 24) return false;
  if (b[0] == 0xFF && b[1] == 0xD8) {
    bool prog = false;
    uint16_t w = 0, h = 0;
    if (!jpegInfo(f, off, len, w, h, prog) || prog) return false;
    c.png = false; c.w = w; c.h = h;
  } else if (!memcmp(b, "\x89PNG\r\n\x1a\n", 8)) {
    const uint32_t w = be32(b + 16), h = be32(b + 20);
    if (!w || !h || w > 2048 || h > 2048 || len > 400UL * 1024UL) return false;
    c.png = true; c.w = (uint16_t)w; c.h = (uint16_t)h;
  } else {
    return false;
  }
  c.off = off; c.len = len; c.valid = true;
  safeCopy(c.path, sizeof(c.path), gProbePath);
  return true;
}

// Descobre onde esta a imagem dentro de um frame APIC/PIC (sem ler a imagem).
bool locateApic(File &f, uint32_t framePos, uint32_t frameSize, int ver,
                uint32_t &imgOff, uint32_t &imgLen, uint8_t &ptype) {
  if (frameSize < 8 || !f.seek(framePos)) return false;
  const size_t n = frameSize < sizeof(gTextBuf) ? frameSize : sizeof(gTextBuf);
  if ((size_t)f.read(gTextBuf, n) != n) return false;
  const uint8_t *b = gTextBuf;
  size_t i = 0;
  const uint8_t enc = b[i++];
  if (ver == 2) i += 3;
  else { while (i < n && b[i]) ++i; ++i; }
  if (i >= n) return false;
  ptype = b[i++];
  if (enc == 1 || enc == 2) {
    while (i + 1 < n && !(b[i] == 0 && b[i + 1] == 0)) i += 2;
    i += 2;
  } else {
    while (i < n && b[i]) ++i;
    ++i;
  }
  if (i > n || i >= frameSize) return false;
  imgOff = framePos + (uint32_t)i;
  imgLen = frameSize - (uint32_t)i;
  return true;
}

// ---- ID3v2 (MP3/AAC). Retorna o byte onde comeca o audio.
uint32_t parseId3(File &f, Meta &m) {
  uint8_t h[10];
  if (!f.seek(0) || f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
  const uint8_t ver = h[3];
  const uint32_t tagEnd = 10 + syncsafe(h + 6);
  const uint32_t audioStart = tagEnd + ((h[5] & 0x10) ? 10 : 0);
  if (ver < 2 || ver > 4) return audioStart;
  const bool unsync = (h[5] & 0x80) != 0;
  const size_t hdr = (ver == 2) ? 6 : 10;
  uint32_t pos = 10;
  if ((h[5] & 0x40) && ver >= 3) {
    uint8_t eh[4];
    if (!f.seek(10) || f.read(eh, 4) != 4) return audioStart;
    pos = (ver == 4) ? 10 + syncsafe(eh) : 14 + be32(eh);
  }
  bool haveTitle = false, haveArtist = false, haveFront = false;

  for (int guard = 0; guard < 200 && pos + hdr <= tagEnd; ++guard) {
    if (haveTitle && haveArtist && haveFront) break;
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
    const uint32_t next = pos + (uint32_t)hdr + fsz;

    uint32_t dpos = pos + (uint32_t)hdr, dsz = fsz;
    bool skip = false;
    if (ver >= 3) {
      const uint8_t ff = fh[9];
      if ((ver == 3 && (ff & 0xC0)) || (ver == 4 && (ff & 0x0C))) skip = true;
      if (ver == 4 && (ff & 0x01)) { if (dsz <= 4) skip = true; else { dpos += 4; dsz -= 4; } }
    }
    if (!skip) {
      const bool isT = (ver == 2) ? !strcmp(id, "TT2") : !strcmp(id, "TIT2");
      const bool isA = (ver == 2) ? !strcmp(id, "TP1") : !strcmp(id, "TPE1");
      const bool isCover = (ver == 2) ? !strcmp(id, "PIC") : !strcmp(id, "APIC");
      if (isCover && !haveFront && !unsync) {
        uint32_t io = 0, il = 0;
        uint8_t pt = 0;
        if (locateApic(f, dpos, dsz, ver, io, il, pt) && (pt == 3 || !coverRef.valid)) {
          CoverRef c;
          if (inspectImage(f, io, il, c)) { coverRef = c; haveFront = (pt == 3); }
        }
      } else if ((isT && !haveTitle) || (isA && !haveArtist)) {
        const size_t take = dsz < sizeof(gTextBuf) ? dsz : sizeof(gTextBuf);
        if (f.seek(dpos) && (size_t)f.read(gTextBuf, take) == take && take > 1) {
          if (isT) {
            decodeText(gTextBuf + 1, take - 1, gTextBuf[0], m.title, sizeof(m.title));
            haveTitle = m.title[0] != 0;
          } else {
            decodeText(gTextBuf + 1, take - 1, gTextBuf[0], m.artist, sizeof(m.artist));
            haveArtist = m.artist[0] != 0;
          }
        }
      }
    }
    pos = next;
  }
  return audioStart;
}

void probeMp3Audio(File &f, Meta &m) {
  uint32_t frames = 0, bitrate = 0, sr = 0, spf = 1152;
  if (!f.seek(m.audioStart)) return;
  const size_t got = f.read(gProbeBuf, sizeof(gProbeBuf));
  int i = findMp3Sync(gProbeBuf, got);
  if (i >= 0) {
    Mp3Hdr h;
    parseMp3Hdr(gProbeBuf + i, h);
    bitrate = h.bitrate; sr = h.sr; spf = h.spf;
    const bool crc = !(gProbeBuf[i + 1] & 1);
    const size_t side = h.mpeg1 ? (h.mono ? 17 : 32) : (h.mono ? 9 : 17);
    const size_t xoff = (size_t)i + 4 + (crc ? 2 : 0) + side;
    bool tagFrame = false;
    if (xoff + 8 <= got && (!memcmp(gProbeBuf + xoff, "Xing", 4) || !memcmp(gProbeBuf + xoff, "Info", 4))) {
      tagFrame = true;
      const uint32_t fl = be32(gProbeBuf + xoff + 4);
      size_t o = xoff + 8;
      if ((fl & 1) && o + 4 <= got) { frames = be32(gProbeBuf + o); o += 4; }
      if (fl & 2) o += 4;
      if ((fl & 4) && o + 100 <= got) { memcpy(m.toc, gProbeBuf + o, 100); m.hasToc = true; }
    } else if ((size_t)i + 36 + 18 <= got && !memcmp(gProbeBuf + i + 36, "VBRI", 4)) {
      tagFrame = true;
      frames = be32(gProbeBuf + i + 36 + 14);
    }
    m.seekBase = m.audioStart + (uint32_t)i;
    m.audioStart = m.seekBase + (tagFrame ? h.frameLen : 0);  // pula o frame de cabecalho
  }

  uint32_t dataEnd = m.fileSize;
  if (m.fileSize > 128 && f.seek(m.fileSize - 128)) {  // ID3v1
    uint8_t t[128];
    if (f.read(t, 128) == 128 && t[0] == 'T' && t[1] == 'A' && t[2] == 'G') {
      dataEnd = m.fileSize - 128;
      if (!m.title[0]) decodeText(t + 3, 30, 0, m.title, sizeof(m.title));
      if (!m.artist[0]) decodeText(t + 33, 30, 0, m.artist, sizeof(m.artist));
    }
  }
  uint32_t dur = 0;
  if (frames > 0 && sr > 0) dur = (uint32_t)((uint64_t)frames * spf / sr);
  else if (bitrate > 0 && dataEnd > m.audioStart)
    dur = (uint32_t)((uint64_t)(dataEnd - m.audioStart) * 8 / ((uint64_t)bitrate * 1000));
  if (dur > 36000) dur = 0;
  m.durationSec = dur;
  if (dataEnd > m.audioStart) m.fileSize = dataEnd;
}

void probeFlac(File &f, Meta &m) {
  uint8_t h[4];
  if (!f.seek(0) || f.read(h, 4) != 4 || memcmp(h, "fLaC", 4) != 0) return;
  uint32_t pos = 4;
  for (int g = 0; g < 24; ++g) {
    uint8_t bh[4];
    if (!f.seek(pos) || f.read(bh, 4) != 4) break;
    const bool last = (bh[0] & 0x80) != 0;
    const int type = bh[0] & 0x7F;
    const uint32_t len = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | bh[3];
    const uint32_t body = pos + 4;
    if (type == 0 && len >= 18) {
      uint8_t s[18];
      if (f.read(s, 18) == 18) {
        const uint32_t sr = ((uint32_t)s[10] << 12) | ((uint32_t)s[11] << 4) | (s[12] >> 4);
        const uint64_t total = ((uint64_t)(s[13] & 0x0F) << 32) | ((uint64_t)s[14] << 24) |
                               ((uint64_t)s[15] << 16) | ((uint64_t)s[16] << 8) | s[17];
        if (sr > 0 && total > 0) {
          const uint64_t d = total / sr;
          m.durationSec = d > 36000 ? 0 : (uint32_t)d;
        }
      }
    } else if (type == 4) {  // VORBIS_COMMENT
      const size_t n = len < sizeof(gProbeBuf) ? len : sizeof(gProbeBuf);
      if (n >= 8 && f.read(gProbeBuf, n) == n) {
        size_t o = 4 + le32(gProbeBuf);
        if (o + 4 <= n) {
          const uint32_t cnt = le32(gProbeBuf + o);
          o += 4;
          for (uint32_t c = 0; c < cnt && o + 4 <= n; ++c) {
            const uint32_t l = le32(gProbeBuf + o);
            o += 4;
            if (l > n - o) break;
            const char *kv = (const char *)gProbeBuf + o;
            if (l > 6 && !strncasecmp(kv, "TITLE=", 6))
              decodeText((const uint8_t *)kv + 6, l - 6, 3, m.title, sizeof(m.title));
            else if (l > 7 && !strncasecmp(kv, "ARTIST=", 7))
              decodeText((const uint8_t *)kv + 7, l - 7, 3, m.artist, sizeof(m.artist));
            o += l;
          }
        }
      }
    } else if (type == 6 && !coverRef.valid) {  // PICTURE
      const size_t n = len < sizeof(gTextBuf) ? len : sizeof(gTextBuf);
      if (n > 40 && f.read(gTextBuf, n) == n) {
        size_t o = 8 + be32(gTextBuf + 4);
        if (o + 4 <= n) {
          o += 4 + be32(gTextBuf + o);
          o += 16;
          if (o + 4 <= n) {
            const uint32_t dl = be32(gTextBuf + o);
            o += 4;
            if (o + dl <= len) {
              CoverRef c;
              if (inspectImage(f, body + (uint32_t)o, dl, c)) coverRef = c;
            }
          }
        }
      }
    }
    pos = body + len;
    if (last) break;
  }
  m.audioStart = m.seekBase = pos;
}

void probeWav(File &f, Meta &m) {
  uint8_t h[12];
  if (!f.seek(0) || f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return;
  uint32_t pos = 12, byteRate = 0, dataSize = 0;
  for (int g = 0; g < 24; ++g) {
    uint8_t ch[8];
    if (!f.seek(pos) || f.read(ch, 8) != 8) break;
    const uint32_t sz = le32(ch + 4);
    if (!memcmp(ch, "fmt ", 4)) {
      uint8_t b[16];
      if (f.read(b, 16) == 16) byteRate = le32(b + 8);
    } else if (!memcmp(ch, "data", 4)) {
      dataSize = sz;
      pos += 8;
      break;
    }
    pos += 8 + sz + (sz & 1);
  }
  if (byteRate) {
    if (!dataSize || dataSize > m.fileSize) dataSize = m.fileSize > pos ? m.fileSize - pos : 0;
    const uint32_t d = dataSize / byteRate;
    m.durationSec = d > 36000 ? 0 : d;
  }
}

// cover.jpg / folder.jpg etc. na pasta (resultado em cache por pasta)
void findFolderCover(const char *path) {
  char dir[PATH_SIZE];
  safeCopy(dir, sizeof(dir), path);
  char *sl = strrchr(dir, '/');
  if (!sl) return;
  if (sl == dir) dir[1] = '\0'; else *sl = '\0';
  static char cachedDir[PATH_SIZE] = "\x01";
  static CoverRef cached;
  if (strcmp(cachedDir, dir) == 0) { coverRef = cached; return; }
  safeCopy(cachedDir, sizeof(cachedDir), dir);
  cached = CoverRef();
  static const char *const NAMES[] = {"cover", "folder", "front", "album", "albumart"};
  static const char *const EXTS[] = {"jpg", "jpeg", "png"};
  bool found = false;
  for (int n = 0; n < 5 && !found; ++n) {
    for (int e = 0; e < 3 && !found; ++e) {
      char nm[24], p[PATH_SIZE];
      snprintf(nm, sizeof(nm), "%s.%s", NAMES[n], EXTS[e]);
      if (!buildPath(p, sizeof(p), dir, nm) || !activeFS().exists(p)) continue;
      File f = activeFS().open(p, FILE_READ);
      if (!f) continue;
      const uint32_t sz = (uint32_t)f.size();
      safeCopy(gProbePath, sizeof(gProbePath), p);
      CoverRef c;
      if (inspectImage(f, 0, sz, c)) { cached = c; found = true; }
      f.close();
    }
  }
  coverRef = cached;
}

void probeMeta(const char *path, Fmt fmt, Meta &m) {
  memset(&m, 0, sizeof(m));
  m.fmt = fmt;
  coverRef = CoverRef();
  safeCopy(gProbePath, sizeof(gProbePath), path);
  File f = activeFS().open(path, FILE_READ);
  if (!f) return;
  m.fileSize = (uint32_t)f.size();
  uint32_t audioStart = 0;
  if (fmt == FMT_MP3 || fmt == FMT_AAC) audioStart = parseId3(f, m);
  m.audioStart = m.seekBase = audioStart;
  if (fmt == FMT_MP3) probeMp3Audio(f, m);
  else if (fmt == FMT_FLAC) probeFlac(f, m);
  else if (fmt == FMT_WAV) probeWav(f, m);
  f.close();
  if (!coverRef.valid) findFolderCover(path);
}

// Leitura da capa em streaming; mantem o audio rodando entre as leituras.
class CoverStream : public lgfx::DataWrapper {
public:
  ~CoverStream() { close(); }
  bool open(const CoverRef &c) {
    f_ = activeFS().open(c.path, FILE_READ);
    off_ = c.off; len_ = c.len; pos_ = 0;
    return f_ && f_.seek(off_);
  }
  int read(uint8_t *buf, uint32_t n) override {
    if (!f_ || pos_ >= len_) return 0;
    if (n > len_ - pos_) n = len_ - pos_;
    const int r = (int)f_.read(buf, n);
    if (r <= 0) return 0;
    pos_ += (uint32_t)r;
    pumpAudio();
    return r;
  }
  void skip(int32_t o) override {
    int64_t np = (int64_t)pos_ + o;
    if (np < 0) np = 0;
    seek((uint32_t)np);
  }
  bool seek(uint32_t o) override {
    if (!f_ || o > len_) return false;
    pos_ = o;
    return f_.seek(off_ + o);
  }
  void close() override { if (f_) f_.close(); }
  int32_t tell() override { return (int32_t)pos_; }

private:
  File f_;
  uint32_t off_ = 0, len_ = 0, pos_ = 0;
};

bool decodeCover() {
  if (!coverCanvasReady || !coverRef.valid) return false;
  coverCanvas.fillSprite(C_PANEL2);
  CoverStream cs;
  if (!cs.open(coverRef)) return false;
  const int big = coverRef.w > coverRef.h ? coverRef.w : coverRef.h;
  float sc = (float)COVER_PX / (float)big;
  if (sc > 1.0f) sc = 1.0f;
  const int dw = (int)(coverRef.w * sc), dh = (int)(coverRef.h * sc);
  const int x = (COVER_PX - dw) / 2, y = (COVER_PX - dh) / 2;
  bool ok;
  if (coverRef.png) ok = coverCanvas.drawPng(&cs, x, y, COVER_PX - x, COVER_PX - y, 0, 0, sc, sc);
  else ok = coverCanvas.drawJpg(&cs, x, y, COVER_PX - x, COVER_PX - y, 0, 0, sc, sc);
  return ok;
}

void prepareCoverState() {
  coverLoaded = false;
  coverPending = coverRef.valid;
  if (coverCanvasReady) coverCanvas.fillSprite(C_PANEL2);
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
bool needRescan = false;
uint32_t lastRemountAt = 0;
File scanHandle;

constexpr int DIR_WALK_DEPTH = 8;
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
  if (!dir && !isAudioFile(base)) return;
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

void abortScan() {
  if (scanHandle) scanHandle.close();
  closeWalkStack();
  scanning = false;
  validatingDirs = false;
  dirProbeActive = false;
}

// Chamada pela fonte de audio quando o SD falha durante a reproducao.
void sdRemount() {
  abortScan();
  SD.end();
  delay(60);
  sdReady = mountSD();
  lastRemountAt = millis();
  needRescan = true;
  showToast(sdReady ? "SD RECUPERADO" : "SD DESCONECTADO");
}

bool startDirValidation(size_t index) {
  dirProbeActive = false;
  if (index >= entryCount || !entries[index].isDir) return false;
  if (!buildPath(walkRootPath, sizeof(walkRootPath), browseDir, entries[index].name))
    return false;
  File root = activeFS().open(walkRootPath);
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
    const bool found = isAudioFile(base);
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
  File next = activeFS().open(childPath);
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
  abortScan();
  validationIndex = 0;
  entryCount = 0;
  selected = 0;
  firstVisible = 0;
  scanOverflow = false;
  scanFolderLimit = false;
  if (strcmp(path, browseDir) != 0) safeCopy(browseDir, sizeof(browseDir), path);
  safeCopy(reselectName, sizeof(reselectName), reselect ? reselect : "");

  if (!storageReady()) { if (screen == SCR_BROWSER) drawBrowser(); return; }
  scanHandle = activeFS().open(browseDir);
  if (!scanHandle || !scanHandle.isDirectory()) {
    if (scanHandle) scanHandle.close();
    showToast("PASTA INVALIDA");
    if (screen == SCR_BROWSER) drawBrowser();
    return;
  }
  scanning = true;
  if (screen == SCR_BROWSER) drawBrowser();
}

void scanStep(uint32_t budgetMs) {
  if (!scanning) return;
  const uint32_t t0 = millis();
  while (scanning && millis() - t0 < budgetMs) {
    if (validatingDirs) {
      if (dirProbeActive) {
        if (walkDepth > 0) { walkDirectoryStep(); continue; }
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
uint32_t trackStartedAt = 0;
int stallRetries = 0;
bool trackEndPending = false;
int pendingSeek = 0;
uint32_t seekDue = 0;
int pendingSkip = 0;
uint32_t skipDue = 0;

uint32_t elapsedSec() {
  return startOffsetSec + (uint32_t)(gSamplesSubmitted / OUT_RATE);
}

bool canSeek() { return meta.fmt == FMT_MP3 && meta.durationSec >= 3; }

AudioGenerator *makeDecoder(Fmt f) {
  switch (f) {
    case FMT_MP3: return new (std::nothrow) AudioGeneratorMP3();
#ifdef HAVE_WAV
    case FMT_WAV: return new (std::nothrow) AudioGeneratorWAV();
#endif
#ifdef HAVE_FLAC
    case FMT_FLAC: return new (std::nothrow) AudioGeneratorFLAC();
#endif
#ifdef HAVE_AAC
    case FMT_AAC: return new (std::nothrow) AudioGeneratorAAC();
#endif
    default: return nullptr;
  }
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
  trackEndPending = false;
  pendingSeek = 0;
  pendingSkip = 0;
  memset(gBand, 0, sizeof(gBand));
  memset(gScope, 0, sizeof(gScope));
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  neopixelWrite(21, 0, 0, 0);
#endif
}

uint32_t mp3SeekByte(uint32_t sec) {
  const uint64_t span = (meta.fileSize > meta.seekBase) ? (meta.fileSize - meta.seekBase) : 0;
  if (!span || !meta.durationSec) return meta.audioStart;
  const float pct = 100.0f * (float)sec / (float)meta.durationSec;
  uint32_t off256;
  if (meta.hasToc) {
    int i = (int)pct;
    if (i > 99) i = 99;
    const float fr = pct - (float)i;
    const float a = meta.toc[i];
    const float b = (i < 99) ? (float)meta.toc[i + 1] : 256.0f;
    off256 = (uint32_t)(a + (b - a) * fr);
  } else {
    off256 = (uint32_t)(256.0f * pct / 100.0f);
  }
  return meta.seekBase + (uint32_t)(span * off256 / 256);
}

bool startTrack(size_t qIdx, uint32_t startSec, bool reprobe, bool natural) {
  if (qIdx >= queueCount) return false;
  char path[PATH_SIZE];
  if (!buildPath(path, sizeof(path), queueDir, queueNames[qIdx])) return false;
  const Fmt fmt = formatOf(queueNames[qIdx]);
  if (fmt == FMT_NONE) return false;

  stopPlayback(!natural);

  if (reprobe) {
    probeMeta(path, fmt, meta);
    prepareCoverState();
    char base[NAME_SIZE];
    toAscii(queueNames[qIdx], base, sizeof(base));
    stripExt(base);
    if (meta.title[0] && meta.artist[0]) snprintf(currentTitle, sizeof(currentTitle), "%s - %s", meta.artist, meta.title);
    else if (meta.title[0]) safeCopy(currentTitle, sizeof(currentTitle), meta.title);
    else if (meta.artist[0]) snprintf(currentTitle, sizeof(currentTitle), "%s - %s", meta.artist, base);
    else safeCopy(currentTitle, sizeof(currentTitle), base);
  }
  safeCopy(currentPath, sizeof(currentPath), path);

  uint32_t startByte = 0, startedAt = 0;
  if (fmt == FMT_MP3) {
    startByte = meta.audioStart;  // pula ID3 e cabecalho Xing
    if (startSec > 0 && canSeek()) {
      if (startSec >= meta.durationSec) startSec = meta.durationSec - 1;
      startByte = alignMp3(path, mp3SeekByte(startSec));
      startedAt = startSec;
    }
  }

  source = new (std::nothrow) SdStreamSource();
  if (!source || !source->open(path)) { stopPlayback(true); return false; }
  if (startByte > 0) source->seek((int32_t)startByte, SEEK_SET);

  bufferedSource = new (std::nothrow) AudioFileSourceBuffer(source, sdAudioBuffer, sizeof(sdAudioBuffer));
  audioOutput = new (std::nothrow) AudioOut();
  decoder = makeDecoder(fmt);
  if (!bufferedSource || !audioOutput || !decoder) { stopPlayback(true); return false; }

  audioOutput->SetGain(1.0f);
  gFadeTarget = 1.0f;
  gFadeGain = natural ? 1.0f : 0.0f;  // fade-in curto, exceto na troca gapless
  if (!natural) { resetDsp(); gVolCur = gVolTarget; }
  gSamplesSubmitted = 0;

  if (!decoder->begin(bufferedSource, audioOutput)) { stopPlayback(true); return false; }

  startOffsetSec = startedAt;
  lastProgress = millis();
  trackStartedAt = millis();
  lastSamples = 0;
  stallRetries = 0;
  trackEndPending = false;
  playing = true;
  paused = false;
  fadeAction = FA_NONE;
  return true;
}

void finishQueue() {
  stopPlayback(false);
  showToast("FIM DA LISTA");
  if (screen == SCR_PLAYER) { screen = SCR_BROWSER; }
  redrawCurrent();
}

bool stepOrder(int dir, bool automatic) {
  const int n = (int)queueCount;
  int np = (int)orderPos + dir;
  if (np >= n) {
    if (automatic && opt.repeat == 0) return false;
    np = 0;
    if (opt.shuffle && n > 1) {
      const uint16_t last = order[orderPos];
      shuffleOrder(-1);
      if (order[0] == last) { const uint16_t t = order[0]; order[0] = order[1]; order[1] = t; }
    }
  }
  if (np < 0) np = n - 1;
  orderPos = (size_t)np;
  return true;
}

// Avanca "steps" faixas na ordem e toca apenas a final.
bool advance(int dir, bool automatic, int steps = 1) {
  if (!queueCount) return false;
  for (int s = 1; s < steps; ++s) stepOrder(dir, false);
  for (int tries = 0; tries < (int)queueCount; ++tries) {
    if (!stepOrder(dir, automatic)) { finishQueue(); return false; }
    if (startTrack(order[orderPos], 0, true, automatic)) { redrawCurrent(); return true; }
  }
  stopPlayback(true);
  showToast("ERRO AO ABRIR ARQUIVO");
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

void requestSkip(int dir) {
  if (!playing) return;
  pendingSkip += dir;
  skipDue = millis() + 200;
  char t[24];
  snprintf(t, sizeof(t), pendingSkip > 0 ? "PROXIMA +%d" : "ANTERIOR %d", pendingSkip);
  showToast(t);
}

void skipStep() {
  if (!pendingSkip || (int32_t)(millis() - skipDue) < 0) return;
  const int n = pendingSkip;
  pendingSkip = 0;
  if (!playing) return;
  advance(n > 0 ? +1 : -1, false, n > 0 ? n : -n);
}

void previousOrRestart() {
  if (!playing) return;
  if (pendingSkip == 0 && elapsedSec() > 3) {
    if (startTrack(order[orderPos], 0, false, false)) redrawCurrent();
    else advance(+1, true);
  } else {
    requestSkip(-1);
  }
}

void seekBy(int delta) {
  if (!playing) return;
  if (!canSeek()) { showToast("BUSCA INDISPONIVEL"); return; }
  pendingSeek += delta;
  seekDue = millis() + 280;
  char t[24];
  snprintf(t, sizeof(t), "BUSCA %+ds", pendingSeek);
  showToast(t);
}

void seekStep() {
  if (!pendingSeek || (int32_t)(millis() - seekDue) < 0) return;
  const int d = pendingSeek;
  pendingSeek = 0;
  if (!playing) return;
  int32_t target = (int32_t)elapsedSec() + d;
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
    fadeAction = FA_NONE;
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

// Decodifica por um tempo limitado. Nao chama onTrackEnded (seguro dentro de outras tarefas).
void decodeSlice(uint32_t budgetMs) {
  if (!playing || paused || !decoder || !audioOutput || trackEndPending) return;
  const uint32_t t0 = millis();
  while (millis() - t0 < budgetMs) {
    gSliceLeft = 2304;
    if (!decoder->loop()) { trackEndPending = true; break; }
    if (audioOutput->isFull()) break;
  }
}

void pumpAudio() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 6) return;
  last = now;
  decodeSlice(4);
}

// ============================================================ desenho
void drawGrid(LovyanGFX &g, int height = 135, int top = 20) {
  const uint16_t minor = rgb(8, 17, 29);
  const uint16_t major = rgb(12, 29, 49);
  for (int x = 8; x < 240; x += 16)
    g.drawFastVLine(x, top, height - top, (x % 32 == 8) ? major : minor);
  for (int y = top + 4; y < height; y += 16)
    g.drawFastHLine(0, y, 240, (y % 32 == 24) ? major : minor);
}

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
  const uint16_t gridMinor = rgb(8, 17, 29);
  const uint16_t gridMajor = rgb(12, 29, 49);
  for (int x = 8; x < 240; x += 16)
    d.drawFastVLine(x, 0, 135, (x % 32 == 8) ? gridMajor : gridMinor);
  for (int y = 8; y < 135; y += 16)
    d.drawFastHLine(0, y, 240, (y % 32 == 8) ? gridMajor : gridMinor);
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
  drawGrid(g);

  char title[NAME_SIZE];
  if (browseDir[1] == '\0') safeCopy(title, sizeof(title), storageKind == STORAGE_SD ? "SD CARD" : "MEMORIA INTERNA");
  else {
    const char *base = strrchr(browseDir, '/');
    toAscii(base ? base + 1 : browseDir, title, sizeof(title));
  }
  drawHeader(g, title, C_CYAN);

  applyUiFont(g);
  g.setTextSize(1);
  constexpr int ROW_H = 17, Y0 = 24;

  if (!storageReady()) {
    g.setTextColor(C_RED);
    g.drawString(storageKind == STORAGE_SD ? uiText(UI_SD_MISSING) : uiText(UI_FLASH_MISSING), 14, 40);
    applyUiFont(g);
    g.setTextColor(C_GREY);
    g.drawString(uiText(UI_STORAGE_KEYS), 14, 62);
  } else if (scanning) {
    g.setTextColor(C_GREY);
    char s[32];
    snprintf(s, sizeof(s), validatingDirs ? uiText(UI_FILTERING) : uiText(UI_READING), (int)entryCount);
    g.drawString(s, 14, 44);
  } else if (!entryCount) {
    g.setTextColor(C_GREY);
    g.drawString(uiText(UI_EMPTY), 14, 44);
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
      if (!e.isDir) stripExt(shown);
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
    const char *st = (paused || fadeAction == FA_PAUSE) ? uiText(UI_PAUSED) : uiText(UI_PLAYING);
    g.setTextColor((paused || fadeAction == FA_PAUSE) ? C_YELLOW : C_GREY);
    g.drawString(st, 232 - g.textWidth(st), 125);
  } else {
    g.setFont(&fonts::Font0);
    g.setTextColor(C_GREY);
    g.drawString(uiText(UI_BROWSER_KEYS1), 8, 101);
    g.drawString(uiText(UI_BROWSER_KEYS2), 8, 113);
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
  drawGrid(g, 90, 0);

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
    g.drawString(flags, 80, 18);
    if (sleepDeadline) {
      const int32_t rem = (int32_t)(sleepDeadline - millis());
      char z[12];
      snprintf(z, sizeof(z), "ZZ %dM", (int)(rem > 0 ? rem / 60000 + 1 : 0));
      g.setTextColor(C_ORANGE);
      g.drawString(z, 138, 18);
    }
    char rate[20];
    snprintf(rate, sizeof(rate), "%s %.1f", FMT_NAMES[meta.fmt <= FMT_NONE ? meta.fmt : FMT_NONE], gSrcRate / 1000.0f);
    g.setTextColor(C_GREY);
    g.drawString(rate, 232 - g.textWidth(rate), 18);
  }

  static float shown[SPEC_BANDS] = {0};
  static float peak[SPEC_BANDS] = {0};
  constexpr int BAR_W = 14, BAR_GAP = 5, SEGS = 10, SEG_H = 3, TOP = 30;
  const bool active = playing && !paused;

  for (int i = 0; i < SPEC_BANDS; ++i) {
    const float lvl = active ? gBand[i] : 0.0f;
    if (lvl > shown[i]) shown[i] = lvl;
    else shown[i] = max(0.0f, shown[i] - 0.05f);
    if (shown[i] > peak[i]) peak[i] = shown[i];
    else peak[i] = max(0.0f, peak[i] - 0.012f);
  }

  if (opt.visual == 3) {
    for (int i = 0; i < SPEC_BANDS; ++i) {  // barras laterais
      const int xb = (i < 6) ? 8 + i * 14 : 152 + (i - 6) * 14;
      const int h = (int)(shown[i] * 40.0f);
      const uint16_t col = (i % 6) < 3 ? C_CYAN : C_BLUE;
      g.fillRect(xb, 70 - h, 10, h, col);
    }
    g.fillRect(96, 26, 46, 46, C_PANEL2);
    g.drawRect(96, 26, 46, 46, C_DIM);
    if (!coverLoaded) {
      g.setFont(&fonts::Font0);
      g.setTextColor(C_GREY);
      const char *msg = coverPending ? "..." : "SEM CAPA";
      g.drawString(msg, 119 - g.textWidth(msg) / 2, 46);
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
  if (opt.visual == 3 && coverLoaded && coverCanvasReady)
    coverCanvas.pushSprite(97, 22 + 27);
}

void drawPlayer() {
  screen = SCR_PLAYER;
  if (screenOff) return;
  auto &g = ui;
  g.fillSprite(C_BG);
  drawGrid(g);
  const bool p = paused || fadeAction == FA_PAUSE;
  drawHeader(g, p ? uiText(UI_PLAYER_PAUSED) : uiText(UI_PLAYER_PLAYING), p ? C_YELLOW : C_CYAN);

  applyUiFont(g);
  g.setTextSize(1);
  g.setTextColor(C_GREY);
  g.drawString(outMode ? uiText(UI_OUTPUT_HEADPHONE) : uiText(UI_OUTPUT_SPEAKER), 8, 117);
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
  g.drawString(uiText(UI_PLAYER_KEYS), 8, 127);

  ui.pushSprite(0, 0);
  drawLive();
}

void drawEq() {
  screen = SCR_EQ;
  if (screenOff) return;
  auto &g = ui;
  g.fillSprite(C_BG);
  drawGrid(g);
  drawHeader(g, uiText(UI_EQ_TITLE), C_CYAN);

  constexpr int CENTER_Y = 66, HALF = 26;
  for (int i = 0; i < EQ_BANDS; ++i) {
    const int cx = 32 + i * 44;
    const bool sel = (i == eqSelected);
    if (sel) g.drawRoundRect(cx - 20, 24, 40, 92, 4, C_RED);

    applyUiFont(g);
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
  g.drawString(uiText(UI_EQ_PRESET), 8, 119);
  g.setTextColor(C_CYAN);
  g.drawString(currentPresetName(), 50, 119);
  g.setTextColor(C_GREY);
  g.drawString(uiText(UI_EQ_KEYS), 8, 127);
  ui.pushSprite(0, 0);
}

constexpr int SETTINGS_COUNT = 11;
const char *const LANGUAGE_NAMES[9] = {
  "ENGLISH", "PORTUGUES (BR)", "ESPANOL", "中文", "日本語",
  "FRANCAIS", "DEUTSCH", "РУССКИЙ", "ITALIANO"
};
const char *const SETTING_LABELS[9][SETTINGS_COUNT] = {
  {"BRIGHTNESS","SCREEN TIMEOUT","SLEEP TIMER","SHAKE TO SKIP","REPEAT","SHUFFLE","VISUALIZER","THEME","SOUND ENHANCEMENT","LANGUAGE","RESTORE DEFAULTS"},
  {"BRILHO","TELA APAGA","TIMER DE SONO","AGITAR = PROXIMA","REPETIR","ALEATORIO","VISUALIZADOR","TEMA","REALCE DE SOM","IDIOMA","RESTAURAR PADROES"},
  {"BRILLO","APAGADO DE PANTALLA","TEMPORIZADOR","AGITAR PARA AVANZAR","REPETIR","ALEATORIO","VISUALIZADOR","TEMA","MEJORA DE SONIDO","IDIOMA","VALORES INICIALES"},
  {"亮度","屏幕超时","睡眠定时器","摇动切歌","循环","随机播放","可视化","主题","声音增强","语言","恢复默认"},
  {"明るさ","画面オフ","スリープタイマー","振って次の曲","リピート","シャッフル","ビジュアライザー","テーマ","サウンド強化","言語","初期設定に戻す"},
  {"LUMINOSITE","VEILLE ECRAN","MINUTERIE","SECOUER POUR SUIVANT","REPETITION","ALEATOIRE","VISUALISEUR","THEME","AMPLIFICATION","LANGUE","REINITIALISER"},
  {"HELLIGKEIT","BILDSCHIRM AUS","SLEEP-TIMER","SCHUETTELN = WEITER","WIEDERHOLEN","ZUFALL","VISUALISIERUNG","THEMA","KLANGVERBESSERUNG","SPRACHE","ZURUECKSETZEN"},
  {"ЯРКОСТЬ","ТАЙМ-АУТ ЭКРАНА","ТАЙМЕР СНА","ВСТРЯХНУТЬ = ДАЛЕЕ","ПОВТОР","ПЕРЕМЕШАТЬ","ВИЗУАЛИЗАТОР","ТЕМА","УЛУЧШЕНИЕ ЗВУКА","ЯЗЫК","СБРОСИТЬ"},
  {"LUMINOSITA","SPEGNIMENTO SCHERMO","SPEGNIMENTO MUSICA","SCUOTI PER AVANTI","RIPETI","CASUALE","VISUALIZZATORE","TEMA","MIGLIORIA AUDIO","LINGUA","RIPRISTINA"}
};

const char *uiLanguageName() { return LANGUAGE_NAMES[constrain(uiLanguage, 0, 8)]; }
// Compact on-device UI dictionary. Audio, filesystem and decoder paths are not affected.
enum UiTextId {
  UI_SD_MISSING, UI_FLASH_MISSING, UI_STORAGE_KEYS, UI_FILTERING, UI_READING,
  UI_EMPTY, UI_PAUSED, UI_PLAYING, UI_BROWSER_KEYS1, UI_BROWSER_KEYS2,
  UI_PLAYER_PAUSED, UI_PLAYER_PLAYING, UI_OUTPUT_HEADPHONE, UI_OUTPUT_SPEAKER,
  UI_PLAYER_KEYS, UI_EQ_TITLE, UI_EQ_PRESET, UI_EQ_KEYS, UI_TEXT_COUNT
};
const char *const UI_TEXT[9][UI_TEXT_COUNT] = {
  {"microSD card not found","Internal storage unavailable","M: storage  |  R: refresh","Filtering folders %d","Reading folder... %d","Empty folder (no audio)","PAUSED","PLAYING","ENTER open/play  DEL parent","S settings  E EQ  R refresh  H help","PAUSED","PLAYING","HEADPHONES","SPEAKER","SPACE pause  N/P track  ,/ seek  TAB EQ","EQUALIZER","PRESET","1-6 presets  0 reset  TAB back"},
  {"Cartao microSD nao encontrado","Memoria interna indisponivel","M: armazenamento  |  R: atualizar","Filtrando pastas %d","Lendo pasta... %d","Pasta vazia (sem audio)","PAUSADO","TOCANDO","ENTER abre/toca  DEL pasta acima","S ajustes  E EQ  R atualizar  H ajuda","PAUSADO","TOCANDO","FONE","ALTO-FALANTE","SPACE pausa  N/P faixa  ,/ busca  TAB EQ","EQUALIZADOR","PRESET","1-6 presets  0 zera  TAB voltar"},
  {"Tarjeta microSD no encontrada","Almacenamiento interno no disponible","M: almacenamiento  |  R: actualizar","Filtrando carpetas %d","Leyendo carpeta... %d","Carpeta vacia (sin audio)","PAUSADO","REPRODUCIENDO","ENTER abrir/reproducir  DEL subir","S ajustes  E EQ  R actualizar  H ayuda","PAUSADO","REPRODUCIENDO","AURICULARES","ALTAVOZ","SPACE pausa  N/P pista  ,/ buscar  TAB EQ","ECUALIZADOR","PREAJUSTE","1-6 presets  0 reiniciar  TAB volver"},
  {"未找到 microSD 卡","内部存储不可用","M：存储  |  R：刷新","正在筛选文件夹 %d","正在读取文件夹... %d","文件夹为空（无音频）","已暂停","正在播放","ENTER 打开/播放  DEL 返回上级","S 设置  E 均衡器  R 刷新  H 帮助","已暂停","正在播放","耳机","扬声器","SPACE 暂停  N/P 切歌  ,/ 搜索  TAB 均衡器","均衡器","预设","1-6 预设  0 重置  TAB 返回"},
  {"microSDカードが見つかりません","内部ストレージを利用できません","M: ストレージ  |  R: 更新","フォルダーを確認中 %d","フォルダーを読み込み中... %d","フォルダーは空です","一時停止","再生中","ENTER 開く/再生  DEL 上へ","S 設定  E EQ  R 更新  H ヘルプ","一時停止","再生中","ヘッドホン","スピーカー","SPACE 一時停止  N/P 曲  ,/ 検索  TAB EQ","イコライザー","プリセット","1-6 プリセット  0 リセット  TAB 戻る"},
  {"Carte microSD introuvable","Stockage interne indisponible","M : stockage  |  R : actualiser","Filtrage des dossiers %d","Lecture du dossier... %d","Dossier vide (aucun audio)","EN PAUSE","LECTURE","ENTER ouvrir/lire  DEL dossier parent","S reglages  E EQ  R actualiser  H aide","EN PAUSE","LECTURE","CASQUE","HAUT-PARLEUR","SPACE pause  N/P piste  ,/ recherche  TAB EQ","EGALISEUR","PRESELECTION","1-6 presets  0 reset  TAB retour"},
  {"microSD-Karte nicht gefunden","Interner Speicher nicht verfuegbar","M: Speicher  |  R: aktualisieren","Ordner werden gefiltert %d","Lese Ordner... %d","Ordner leer (keine Audiodateien)","PAUSE","WIEDERGABE","ENTER oeffnen/abspielen  DEL zurueck","S Einstellungen  E EQ  R aktualisieren  H Hilfe","PAUSE","WIEDERGABE","KOPFHOERER","LAUTSPRECHER","SPACE Pause  N/P Titel  ,/ Suche  TAB EQ","EQUALIZER","VOREINSTELLUNG","1-6 Presets  0 reset  TAB zurueck"},
  {"Карта microSD не найдена","Внутренняя память недоступна","M: память  |  R: обновить","Фильтрация папок %d","Чтение папки... %d","Папка пуста (нет аудио)","ПАУЗА","ВОСПРОИЗВЕДЕНИЕ","ENTER открыть/играть  DEL выше","S настройки  E EQ  R обновить  H помощь","ПАУЗА","ВОСПРОИЗВЕДЕНИЕ","НАУШНИКИ","ДИНАМИК","SPACE пауза  N/P трек  ,/ поиск  TAB EQ","ЭКВАЛАЙЗЕР","ПРЕСЕТ","1-6 пресеты  0 сброс  TAB назад"},
  {"Scheda microSD non trovata","Memoria interna non disponibile","M: memoria  |  R: aggiorna","Filtro cartelle %d","Lettura cartella... %d","Cartella vuota (nessun audio)","IN PAUSA","IN RIPRODUZIONE","ENTER apri/riproduci  DEL indietro","S impostazioni  E EQ  R aggiorna  H aiuto","IN PAUSA","IN RIPRODUZIONE","CUFFIE","ALTOPARLANTE","SPACE pausa  N/P brano  ,/ cerca  TAB EQ","EQUALIZZATORE","PRESET","1-6 preset  0 reset  TAB indietro"}
};
const char *uiText(UiTextId id) {
  return UI_TEXT[constrain(uiLanguage, 0, 8)][id];
}

void applyUiFont(LovyanGFX &g, bool small = false) {
  (void)small;
  if (uiLanguage == 3) g.setFont(&fonts::efontCN_10);
  else if (uiLanguage == 4) g.setFont(&fonts::efontJA_10);
  else g.setFont(&fonts::Font0);
}


const char *localizedOn() {
  static const char *v[9] = {"ON","LIGADO","ACTIVADO","开启","オン","ACTIVE","EIN","ВКЛ","ATTIVO"};
  return v[constrain(uiLanguage, 0, 8)];
}
const char *localizedOff() {
  static const char *v[9] = {"OFF","DESLIGADO","DESACTIVADO","关闭","オフ","DESACTIVE","AUS","ВЫКЛ","DISATTIVO"};
  return v[constrain(uiLanguage, 0, 8)];
}
const char *localizedNever() {
  static const char *v[9] = {"NEVER","NUNCA","NUNCA","永不","なし","JAMAIS","NIE","НИКОГДА","MAI"};
  return v[constrain(uiLanguage, 0, 8)];
}
const char *localizedRepeat(int v) {
  static const char *names[9][3] = {
    {"OFF","ALL","ONE"},{"DESLIGADO","TODAS","UMA"},{"DESACTIVADO","TODAS","UNA"},
    {"关闭","全部","单曲"},{"オフ","全曲","1曲"},{"DESACTIVE","TOUT","UN"},
    {"AUS","ALLE","EINZELN"},{"ВЫКЛ","ВСЕ","ОДИН"},{"DISATTIVO","TUTTE","UNA"}
  };
  return names[constrain(uiLanguage,0,8)][constrain(v,0,2)];
}
const char *localizedVisual(int v) {
  static const char *names[9][4] = {
    {"BARS","MIRROR","WAVE","COVER"},{"BARRAS","ESPELHO","ONDA","CAPA"},{"BARRAS","ESPEJO","ONDA","PORTADA"},
    {"柱状","镜像","波形","封面"},{"バー","ミラー","波形","カバー"},{"BARRES","MIROIR","ONDE","POCHETTE"},
    {"BALKEN","SPIEGEL","WELLE","COVER"},{"СТОЛБЦЫ","ЗЕРКАЛО","ВОЛНА","ОБЛОЖКА"},{"BARRE","SPECCHIO","ONDA","COPERTINA"}
  };
  return names[constrain(uiLanguage,0,8)][constrain(v,0,3)];
}
const char *localizedEnhance(int v) {
  static const char *names[9][3] = {
    {"OFF","SOFT","STRONG"},{"DESLIGADO","SUAVE","FORTE"},{"DESACTIVADO","SUAVE","FUERTE"},
    {"关闭","柔和","强劲"},{"オフ","弱","強"},{"DESACTIVE","DOUX","FORT"},
    {"AUS","SANFT","STARK"},{"ВЫКЛ","МЯГКО","СИЛЬНО"},{"DISATTIVO","MORBIDO","FORTE"}
  };
  return names[constrain(uiLanguage,0,8)][constrain(v,0,2)];
}

void settingValue(int i, char *out, size_t n) {
  switch (i) {
    case 0: snprintf(out, n, "%d%%", opt.brightness); break;
    case 1:
      if (SCREEN_OFF_SEC[opt.screenOffIdx]) snprintf(out, n, "%ds", SCREEN_OFF_SEC[opt.screenOffIdx]);
      else safeCopy(out, n, localizedNever());
      break;
    case 2:
      if (SLEEP_MIN[sleepIdx]) snprintf(out, n, "%d MIN", SLEEP_MIN[sleepIdx]);
      else safeCopy(out, n, localizedOff());
      break;
    case 3: safeCopy(out, n, imuOk ? (opt.shake ? localizedOn() : localizedOff()) : "N/A"); break;
    case 4: safeCopy(out, n, localizedRepeat(opt.repeat)); break;
    case 5: safeCopy(out, n, opt.shuffle ? localizedOn() : localizedOff()); break;
    case 6: safeCopy(out, n, localizedVisual(opt.visual)); break;
    case 7: safeCopy(out, n, THEMES[themeIndex].name); break;
    case 8: safeCopy(out, n, localizedEnhance(opt.enhance)); break;
    case 9: safeCopy(out, n, uiLanguageName()); break;
    default: safeCopy(out, n, "ENTER"); break;
  }
}

void drawSettings() {
  screen = SCR_SETTINGS;
  if (screenOff) return;
  auto &g = ui;
  g.fillSprite(C_BG);
  drawGrid(g);
  drawHeader(g, SETTING_LABELS[constrain(uiLanguage, 0, 8)][9], C_CYAN);
  applyUiFont(g, true);
  g.setTextSize(1);
  for (int i = 0; i < SETTINGS_COUNT; ++i) {
    const int y = 22 + i * 9;
    const bool sel = (i == settingsSel);
    if (sel) g.fillRoundRect(6, y - 1, 228, 10, 3, C_PANEL);
    g.setTextColor(sel ? C_WHITE : C_GREY);
    char label[28];
    fitText(g, SETTING_LABELS[constrain(uiLanguage, 0, 8)][i], 128, label, sizeof(label));
    g.drawString(label, 12, y);
    char v[24], shown[32];
    settingValue(i, v, sizeof(v));
    if (sel && i < SETTINGS_COUNT - 1) snprintf(shown, sizeof(shown), "< %s >", v);
    else safeCopy(shown, sizeof(shown), v);
    g.setTextColor(sel ? C_CYAN : C_GREY);
    char valueFit[24];
    fitText(g, shown, 82, valueFit, sizeof(valueFit));
    g.drawString(valueFit, 228 - g.textWidth(valueFit), y);
  }
  g.setTextColor(C_GREY);
  g.drawString(uiLanguage == 1 ? "; . ESCOLHE  , / MUDA  ESC VOLTA" : uiLanguage == 2 ? "; . ELEGIR  , / CAMBIAR  ESC VOLVER" : uiLanguage == 3 ? "; . 选择  , / 更改  ESC 返回" : uiLanguage == 4 ? "; . 選択  , / 変更  ESC 戻る" : uiLanguage == 5 ? "; . CHOISIR  , / MODIFIER  ESC RETOUR" : uiLanguage == 6 ? "; . WAEHLEN  , / AENDERN  ESC ZURUECK" : uiLanguage == 7 ? "; . ВЫБРАТЬ  , / ИЗМЕНИТЬ  ESC НАЗАД" : uiLanguage == 8 ? "; . SCEGLI  , / CAMBIA  ESC INDIETRO" : "; . SELECT  , / CHANGE  ESC BACK", 8, 127);
  ui.pushSprite(0, 0);
}

void drawHelp() {
  screen = SCR_HELP;
  if (screenOff) return;
  static const char *const pages[3][10] = {
    {
      "LIBRARY / GENERAL",
      ";/. move item   ,// page",
      "ENTER open folder or play queue",
      "DEL parent folder  TAB player",
      "SPACE pause  N/P track",
      "- / = volume  1-6 EQ preset",
      "S settings  E equalizer",
      "R refresh SD  X stop audio",
      "G sound enhancement (off/soft/strong)",
      "Formats: MP3 WAV FLAC AAC",
    },
    {
      "PLAYER",
      "SPACE pause | N next track",
      "P previous / restart (>3s)",
      ", / / seek -10s / +10s (MP3)",
      "; / . volume +/- 5%",
      "S shuffle | R repeat off/all/one",
      "V bars/mirror/wave/cover",
      "T sleep timer | O speaker/headphones",
      "TAB/E equalizer | G enhancement",
      "DEL or ` back to library; X stop",
    },
    {
      "EQ / SETTINGS",
      "EQ: , / select band",
      "EQ: ; / . gain +/-",
      "EQ: 0 FLAT | 1-6 presets",
      "SETTINGS: ; / . select item",
      "SETTINGS: , / / change value",
      "ENTER change / restore defaults",
      "TAB/DEL/` return to previous screen",
      "Display off: press any key to wake",
      "H restarts | TAB/DEL closes help",
    },
  };

  auto &g = ui;
  g.fillSprite(C_BG);
  drawGrid(g);
  char title[24];
  snprintf(title, sizeof(title), "HELP %d/3", helpPage + 1);
  drawHeader(g, title, C_CYAN);
  g.setFont(&fonts::Font0);
  g.setTextSize(1);
  for (int i = 0; i < 10; ++i) {
    g.setTextColor(i == 0 ? C_RED : C_GREY);
    g.drawString(pages[helpPage][i], i == 0 ? 8 : 12, 26 + i * 10);
  }
  g.setTextColor(C_CYAN);
  g.drawString("H NEXT  TAB/DEL/` CLOSE", 8, 127);
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
    case 'n': requestSkip(+1); return true;
    case 'p': previousOrRestart(); return true;
    case '=': case '+': changeVolume(+5); return true;
    case '-': case '_': changeVolume(-5); return true;
    case 'g': {
      opt.enhance = (opt.enhance + 1) % 3;
      configureDsp();
      markSettingsDirty();
      char t[28];
      snprintf(t, sizeof(t), "REALCE: %s", ENHANCE_NAMES[opt.enhance]);
      showToast(t);
      redrawCurrent();
      return true;
    }
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
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  const uint32_t freqs[] = {4000000, 2000000, 1000000};
  for (uint32_t f : freqs) {
    if (SD.begin(SD_CS, sdSPI, f, "/sd", 8, false)) {
      Serial.printf("SD montado: freq=%lu Hz, tipo=%u, bytes=%llu\n", (unsigned long)f, (unsigned)SD.cardType(), (unsigned long long)SD.cardSize());
      return true;
    }
    Serial.printf("Falha SD.begin em %lu Hz\n", (unsigned long)f);
    SD.end();
    delay(50);
  }
  return false;
}

void refreshBrowser() {
  if (!playing && storageKind == STORAGE_SD) {  // remonta o cartao apenas se nada esta tocando
    abortScan();
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
  uiLanguage = 0;
  opt = Options();
  themeIndex = 3;
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
  showToast("DEFAULTS RESTORED");
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
    case 8: opt.enhance = (opt.enhance + dir + 3) % 3; configureDsp(); break;
    case 9: uiLanguage = (uiLanguage + dir + 9) % 9; break;
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
    case 'm':
      if (playing) { showToast("PARE A MUSICA PARA TROCAR"); return; }
      abortScan(); storageKind = (storageKind == STORAGE_SD) ? STORAGE_INTERNAL : STORAGE_SD;
      safeCopy(browseDir, sizeof(browseDir), "/"); selected = firstVisible = 0;
      showToast(storageReady() ? (storageKind == STORAGE_SD ? "ARMAZENAMENTO: SD" : "ARMAZENAMENTO: INTERNO") : "ARMAZENAMENTO INDISPONIVEL");
      beginScan("/"); return;
    case 'c': {
      if (storageKind != STORAGE_SD || !sdReady || !internalReady || entryCount == 0 || entries[selected].isDir) { showToast("SELECIONE ARQUIVO NO SD"); return; }
      char src[PATH_SIZE], dst[PATH_SIZE];
      if (!buildPath(src, sizeof(src), browseDir, entries[selected].name)) { showToast("CAMINHO INVALIDO"); return; }
      safeCopy(dst, sizeof(dst), src);
      char parent[PATH_SIZE]; safeCopy(parent, sizeof(parent), dst);
      for (char *q = parent + 1; *q; ++q) {
        if (*q == '/') { *q = 0; LittleFS.mkdir(parent); *q = '/'; }
      }
      File in = SD.open(src, FILE_READ); if (LittleFS.exists(dst)) LittleFS.remove(dst); File out = LittleFS.open(dst, FILE_WRITE);
      if (!in || !out) { if (in) in.close(); if (out) out.close(); showToast("FALHA AO COPIAR"); return; }
      uint8_t buf[1024]; bool ok = true;
      while (in.available()) { size_t n = in.read(buf, sizeof(buf)); if (!n || out.write(buf, n) != n) { ok = false; break; } yield(); }
      in.close(); out.close();
      showToast(ok ? "COPIADO PARA MEMORIA INTERNA" : "ERRO NA COPIA"); return;
    }
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

void setPlaybackSpeed(float speed) {
  playbackSpeed = constrain(speed, 0.75f, 1.75f);
  // Reconfigura apenas a etapa do reamostrador. Nao reinicia decoder,
  // arquivo ou fila do speaker; evita corte ao mudar velocidade.
  rsActive = (fabsf(gSrcRate - (float)OUT_RATE) > 0.5f) ||
             (fabsf(playbackSpeed - 1.0f) > 0.001f);
  rsStep = (gSrcRate / (float)OUT_RATE) * playbackSpeed;
  rsPhase = 0.0f;
  const char *mode = "NORMAL";
  if (fabsf(playbackSpeed - 0.75f) < 0.001f) mode = "SLOWED";
  else if (fabsf(playbackSpeed - 1.25f) < 0.001f) mode = "SPED UP";
  else if (fabsf(playbackSpeed - 1.75f) < 0.001f) mode = "Nightcore";
  char msg[28];
  snprintf(msg, sizeof(msg), "%s", mode);
  showToast(msg);
}

void playerKey(int k) {
  if (mediaKey(k)) return;
  char t[28];
  switch (k) {
    case K_TAB:
    case 'e': returnScreen = SCR_PLAYER; drawEq(); break;
    case K_DEL:
    case '`': drawBrowser(); break;
    case '7': setPlaybackSpeed(0.75f); break;
    case '8': setPlaybackSpeed(1.25f); break;
    case '9': setPlaybackSpeed(1.75f); break;
    case '0': setPlaybackSpeed(1.00f); break;
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
    configureDsp();
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
    requestSkip(+1);
  }
}

void decodeStep() {
  if (!playing || paused || !decoder) return;
  if (!trackEndPending) decodeSlice(8);  // fatia curta: UI e teclado nunca ficam presos
  if (trackEndPending) { trackEndPending = false; onTrackEnded(); return; }
  if (audioOutput && audioOutput->isFull()) delay(2);

  // anti-travamento: sem progresso = arquivo ruim/SD com problema
  if (gSamplesSubmitted != lastSamples) {
    lastSamples = gSamplesSubmitted;
    lastProgress = millis();
    stallRetries = 0;
  } else if (millis() - lastProgress > 4000) {
    lastProgress = millis();
    if (stallRetries == 0 && canSeek()) {  // 1a tentativa: reabre no mesmo ponto
      stallRetries = 1;
      showToast("RECUPERANDO...");
      if (startTrack(order[orderPos], elapsedSec(), false, false)) { redrawCurrent(); return; }
    }
    showToast("ERRO DE LEITURA");
    advance(+1, true);
  }
}

void coverStep() {
  if (!coverPending || !playing || screenOff) return;
  if (screen != SCR_PLAYER || opt.visual != 3) return;
  if (millis() - trackStartedAt < 500) return;  // deixa o buffer de audio encher antes
  coverPending = false;
  coverLoaded = decodeCover();
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

  // apos uma recuperacao de SD, refaz a listagem da pasta atual
  if (needRescan && !scanning && now - lastRemountAt > 1500) {
    needRescan = false;
    if (storageReady()) beginScan(browseDir);
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
  coverCanvasReady = coverCanvas.createSprite(COVER_PX, COVER_PX);
  if (!coverCanvasReady) Serial.println("AVISO: capa indisponivel (sem memoria)");

  // Speaker: 44.1 kHz nativo, mais DMA, tarefa de audio no core 0
  M5Cardputer.Speaker.end();
  auto spk = M5Cardputer.Speaker.config();
  spk.sample_rate = OUT_RATE;
  spk.dma_buf_count = 8;
  spk.dma_buf_len = 512;
  spk.task_pinned_core = 0;
  M5Cardputer.Speaker.config(spk);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(220);

  gVolTarget = 0.5f;
  updateVolumeTarget();
  gVolCur = gVolTarget;
  configureSpectrum();

  imuOk = M5.Imu.isEnabled();
  batteryPercent = M5Cardputer.Power.getBatteryLevel();
  lastActivity = millis();

  internalReady = LittleFS.begin(false);
  sdReady = mountSD();
  storageKind = sdReady ? STORAGE_SD : STORAGE_INTERNAL;
  screen = SCR_BROWSER;
  while (millis() - splashStart < 900) delay(10);
  beginScan((storageReady() && activeFS().exists("/music")) ? "/music" : "/");

  Serial.printf("MP3 PLAYER %s | SD=%s | INTERNAL=%s | IMU=%s | heap=%u maxblk=%u\n", MP3_PLAYER_VERSION,
                sdReady ? "OK" : "FAIL", internalReady ? "OK" : "FAIL", imuOk ? "OK" : "N/D",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void loop() {
  M5Cardputer.update();
  decodeStep();
  fadeStep();
  seekStep();
  skipStep();
  scanStep(4);
  handleKeyboard();
  coverStep();
  pollShake();
  housekeeping();
  ledUpdate();
  delay(1);
}