#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <ctype.h>
#include <AudioOutput.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceICYStream.h>
#include <AudioGeneratorMP3.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <time.h>
#include "johnny_version.h"


constexpr size_t JOHNNY_EQ_BANDS=5;
constexpr float JOHNNY_EQ_FREQUENCIES[JOHNNY_EQ_BANDS]={80.0f,250.0f,1000.0f,4000.0f,10000.0f};
float audioEqGainDb[JOHNNY_EQ_BANDS]={2.5f,1.5f,0.0f,-1.0f,-1.5f};
constexpr size_t RADIO_RECORD_BLOCK_SAMPLES=256;
struct RadioRecordBlock { uint16_t count; int16_t samples[RADIO_RECORD_BLOCK_SAMPLES]; };
QueueHandle_t radioRecordQueue=nullptr;
volatile bool radioRecording=false,radioRecordStartRequested=false,radioRecordStopRequested=false,radioRecordFailed=false,radioRecorderTaskReady=false;
volatile uint8_t radioRecordState=0;
volatile uint32_t radioRecordSampleRate=0,radioRecordDataBytes=0;
String radioRecordPath;

class JohnnyEqualizer {
public:
  void setBand(size_t index,float gainDb) {
    if(index>=JOHNNY_EQ_BANDS)return;
    gains_[index]=constrain(gainDb,-6.0f,6.0f);dirty_=true;
  }
  void setAll(const float* gains) {
    for(size_t i=0;i<JOHNNY_EQ_BANDS;i++)gains_[i]=constrain(gains[i],-6.0f,6.0f);
    dirty_=true;
  }
  void reset() {
    for(size_t i=0;i<JOHNNY_EQ_BANDS;i++){filters_[i].z1=0.0f;filters_[i].z2=0.0f;}
  }
  int16_t processMono(int32_t input,uint32_t sampleRate) {
    if(sampleRate<8000U)sampleRate=22050U;
    if(dirty_||sampleRate_!=sampleRate)rebuild(sampleRate);
    float value=(float)input*preamp_;
    for(size_t i=0;i<JOHNNY_EQ_BANDS;i++){
      Biquad& f=filters_[i];
      const float out=f.b0*value+f.z1;
      f.z1=f.b1*value-f.a1*out+f.z2;
      f.z2=f.b2*value-f.a2*out;
      value=out;
    }
    if(value>28500.0f)value=28500.0f+(value-28500.0f)*0.18f;
    else if(value< -28500.0f)value= -28500.0f+(value+28500.0f)*0.18f;
    value=constrain(value,-32768.0f,32767.0f);
    return (int16_t)lrintf(value);
  }
private:
  struct Biquad {float b0=1.0f,b1=0.0f,b2=0.0f,a1=0.0f,a2=0.0f,z1=0.0f,z2=0.0f;};
  Biquad filters_[JOHNNY_EQ_BANDS];
  float gains_[JOHNNY_EQ_BANDS]={2.5f,1.5f,0.0f,-1.0f,-1.5f};
  uint32_t sampleRate_=0;
  float preamp_=1.0f;
  bool dirty_=true;
  void rebuild(uint32_t sampleRate) {
    sampleRate_=sampleRate;
    float maxBoost=0.0f;
    for(size_t i=0;i<JOHNNY_EQ_BANDS;i++)maxBoost=max(maxBoost,gains_[i]);
    preamp_=powf(10.0f,-(maxBoost*0.72f)/20.0f);
    for(size_t i=0;i<JOHNNY_EQ_BANDS;i++){
      const float frequency=min(JOHNNY_EQ_FREQUENCIES[i],(float)sampleRate*0.44f);
      const float omega=6.28318530718f*frequency/(float)sampleRate;
      const float cosine=cosf(omega),sine=sinf(omega),q=0.82f;
      const float alpha=sine/(2.0f*q),A=powf(10.0f,gains_[i]/40.0f),a0=1.0f+alpha/A;
      Biquad& f=filters_[i];
      f.b0=(1.0f+alpha*A)/a0;f.b1=(-2.0f*cosine)/a0;f.b2=(1.0f-alpha*A)/a0;
      f.a1=(-2.0f*cosine)/a0;f.a2=(1.0f-alpha/A)/a0;
    }
    dirty_=false;
  }
};
JohnnyEqualizer audioEqualizer;

class JohnnyAudioOutput final : public AudioOutput {
public:
  explicit JohnnyAudioOutput(m5::Speaker_Class* speaker) : speaker_(speaker) {}
  bool begin() override { return speaker_ != nullptr; }
  bool ConsumeSample(int16_t sample[2]) override {
    if(!speaker_)return false;
    if(bufferIndex_>=kBufferSamples)flush();
    const int32_t rawLeft=sample[LEFTCHANNEL],rawRight=sample[RIGHTCHANNEL];
    const int16_t equalized=audioEqualizer.processMono((rawLeft+rawRight)/2,hertz);
    // Record after EQ but before the listening-volume control.
    if(radioRecording&&radioRecordQueue){
      if(radioRecordSampleRate==0)radioRecordSampleRate=hertz;
      recordSamples_[recordIndex_++]=equalized;
      if(recordIndex_>=RADIO_RECORD_BLOCK_SAMPLES)sendRecordBlock();
    }
    buffers_[bufferSlot_][bufferIndex_++]=(int16_t)Amplify(equalized);
    return true;
  }
  void flush() override {
    if(!speaker_||bufferIndex_==0)return;
    // Bound output back-pressure so MP3 decoding cannot monopolize the audio mutex.
    uint32_t started=millis();bool queued=false;
    // Never truncate a DMA block to make room for the next decoder block.
    while((uint32_t)(millis()-started)<120UL){if(speaker_->playRaw(buffers_[bufferSlot_],bufferIndex_,hertz,false,1,0,false)){queued=true;break;}vTaskDelay(pdMS_TO_TICKS(1));}
    if(queued)bufferSlot_=(bufferSlot_+1)%3;bufferIndex_=0;
  }
  void flushRecording(){if(recordIndex_>0)sendRecordBlock();}
  void resetRecordingBuffer(){recordIndex_=0;}
  bool stop() override {
    // Control-path stop must not wait for the output buffer to drain.
    bufferIndex_=0;
    if(speaker_) speaker_->stop();
    return true;
  }
private:
  static constexpr size_t kBufferSamples=1024;
  m5::Speaker_Class* speaker_=nullptr;
  int16_t buffers_[3][kBufferSamples]{};
  size_t bufferIndex_=0;
  uint8_t bufferSlot_=0;
  int16_t recordSamples_[RADIO_RECORD_BLOCK_SAMPLES]{};
  size_t recordIndex_=0;
  void sendRecordBlock(){
    if(recordIndex_==0)return;
    RadioRecordBlock block{};block.count=(uint16_t)recordIndex_;
    memcpy(block.samples,recordSamples_,recordIndex_*sizeof(int16_t));
    if(!radioRecordQueue||xQueueSend(radioRecordQueue,&block,0)!=pdTRUE){
      radioRecording=false;radioRecordStopRequested=true;radioRecordFailed=true;radioRecordState=3;
    }
    recordIndex_=0;
  }
};


namespace {
constexpr int SD_CS = 12, SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14;
uint16_t BG = 0x1008, PANEL = 0x280C, UI_PURPLE = 0xA01F, UI_PINK = 0xF19F, UI_LAVENDER = 0xD5BF, UI_MUTED = 0xB5B4;
uint16_t UI_GRID=0x180B, UI_EDGE=0x4933, UI_PANEL_HI=0x3414, UI_CYAN=0x5DDF;
enum Screen : uint8_t { HOME, MUSICMENU, SYSTEMMENU, GAMESMENU, HACKMENU, HACKFX, BLESCAN, HACKINFO, HACKINPUT, PIANO, TUNER, FILES, MUSIC, RADIO, NETWORK, USBMODE, EQSCREEN, LIFE, ABOUT, UPDATEINFO, MICREC, CLOCKSCREEN, DISKMANAGER, PROCESSINFO, THEMES };
Screen eqReturnScreen=HOME;
Screen appReturnMenu=MUSICMENU;
int appReturnSelected=0;
int categoryScroll=0;
int eqSelectedBand=0,eqPreset=1;
const char* eqPresetNames[]={"FLAT","WARM","VOCAL","BASS"};
const float eqPresets[4][JOHNNY_EQ_BANDS]={{0,0,0,0,0},{2.5f,1.5f,0,-1.0f,-1.5f},{-2.0f,-1.0f,2.5f,2.0f,0.5f},{4.0f,2.0f,0,-1.0f,-2.0f}};
constexpr int LIFE_COLS=48,LIFE_ROWS=16;
bool lifeGrid[LIFE_ROWS][LIFE_COLS]{},lifeNext[LIFE_ROWS][LIFE_COLS]{};
bool lifeRunning=true;
int lifeCursorX=LIFE_COLS/2,lifeCursorY=LIFE_ROWS/2;
uint32_t lifeGeneration=0,lifeLastStep=0;
uint16_t lifeIntervalMs=180;
Screen screen = HOME;
SPIClass sdBus(FSPI);
bool sdReady = false;
String currentPath = "/";
String entries[96];
String entryPaths[96];
bool entryIsDir[96];
int entryCount = 0, selected = 0, scroll = 0, tunerInstrument = 0;
int pianoOctave=0,pianoEffect=0,pianoVolume=85;
bool musicNowPlaying=false;
int radioSelected=0;
bool radioEditingUrl=false;
String radioEntry;
String radioMessage="SELECT A STATION";
String audioStatusMessage="STOPPED";
String audioNowTitle;
String audioTrackPath, audioTrackExtension, audioTrackArtist, audioTrackAlbum, audioTrackTitle;
static uint8_t audioCoverData[12U*1024U];
alignas(4) static uint8_t audioDecodeBuffer[12U*1024U];
alignas(8) static uint8_t mp3DecoderWorkspace[AudioGeneratorMP3::preAllocSize()];
uint32_t musicProgressLastDraw=0;
uint8_t musicProgressLastValue=255;
uint32_t audioCoverSize=0;
bool audioCoverPng=false;
bool audioShuffle=false;
uint8_t audioRepeatMode=0;
int audioVolume=14;
AudioFileSource* audioRawSource=nullptr;
AudioFileSourceBuffer* audioInputBuffer=nullptr;
AudioGenerator* audioDecoder=nullptr;
JohnnyAudioOutput* audioOutput=nullptr;
Preferences preferences;
String savedSsid, savedPassword, wifiEntry;
uint8_t themeMode=0,themeSelection=0;
bool clockNtpStarted=false;
bool alarmEnabled=false,alarmActive=false,alarmTriedFile=false,alarmSoundStarted=false;
uint8_t alarmHour=7,alarmMinute=0,alarmWeekdayMask=0x7F;
int32_t alarmLastDayKey=-1;
uint64_t alarmSnoozeUntilEpoch=0;
uint32_t alarmSnoozeUntilMs=0,lastAlarmToneMs=0,lastClockDraw=0,chargingDetectedAt=0;
bool chargingSaverActive=false,chargingSaverDismissed=false;
String alarmSoundPath;TaskHandle_t johnnyAudioTaskHandle=nullptr,johnnyRecorderTaskHandle=nullptr;
uint8_t wifiConfigStage=0;
int lastWiFiStatus=-1;
int lastAboutWiFiStatus=-1;
String wifiNetworks[20];
int wifiNetworkRSSI[20];
int wifiNetworkAuth[20];
int wifiNetworkCount=0, wifiSelected=0, wifiScroll=0;
bool wifiScanPending=false;
bool wifiScanOnly=false;
bool wifiSurveyDetails=false;
int wifiNetworkChannel[20]={};
String wifiNetworkBssid[20];
WebServer johnnyWeb(80);
bool johnnyWebStarted=false,johnnyMdnsStarted=false;
BLEScan* bleScanner=nullptr;
bool bleInitialized=false;
volatile bool bleScanDone=false;
bool bleScanning=false;
String bleNames[8];
int bleRssi[8];
int bleCount=0;
String hackMessage;
String hackInputValue, hackResultMessage;
uint8_t hackInputMode=0;
bool hackInputDone=false;
uint32_t tunerLastSample=0;
float tunerFrequency=0.0f;
int16_t tunerSamples[3072];
float tunerFilteredFrequency=0.0f;
bool tunerHasSignal=false;
uint32_t tunerLastSignalAt=0;
bool usbActive=false;
String usbCommandLine;
bool micStarted=false;
uint32_t micWarmupUntil=0;
constexpr size_t SYNTH_SAMPLES=4096;
constexpr uint8_t SYNTH_AUDIO_CHANNEL=2;
constexpr size_t BOOT_JINGLE_SAMPLES=8192;
static int16_t bootJingleBuffer[BOOT_JINGLE_SAMPLES];
static int16_t alarmToneSamples[512];
static int16_t synthPadSamples[8192];
volatile int8_t synthPadRequest=-1;
uint8_t synthPadSelected=0;
int8_t micRecordPadIndex=-1;
static int16_t synthAudioBuffers[3][SYNTH_SAMPLES];
static float synthSineTable[1024];
uint8_t synthAudioBufferIndex=0;
bool synthSineReady=false;
const char* effectNames[]={"CLEAN","WARM","DRIVE","FUZZ","SQUARE","PAD","BELL","BASS"};
String selectedFile;
volatile bool audioStarted = false;
volatile bool audioPaused = false;
volatile bool audioIsRadio = false;
SemaphoreHandle_t audioMutex = nullptr;
volatile bool audioTaskEnded = false;
volatile bool audioStopRequested = false;
volatile bool audioTaskEndedWasRadio = false;
volatile bool audioTaskReady = false;
constexpr int RADIO_STATION_COUNT=4;
const char* radioStationNames[RADIO_STATION_COUNT]={"RADIO PARADISE","GROOVE SALAD","DRONE ZONE","SPACE STATION"};
const char* radioStationUrls[RADIO_STATION_COUNT]={
  "http://stream-tx1.radioparadise.com/mp3-128",
  "http://ice5.somafm.com/groovesalad-128-mp3",
  "http://ice5.somafm.com/dronezone-128-mp3",
  "http://ice5.somafm.com/spacestation-128-mp3"
};
uint32_t batteryLastDraw = 0;
uint32_t batteryLastSample = 0;
int batteryCachedPercent = -1;
int16_t batteryCachedMillivolts = -1;
bool batteryCachedCharging = false;
bool batteryCacheReady = false;
bool screenSaverActive = false;
const char* apps[] = {"HACK","GAMES","MUSIC","SYSTEM"};
constexpr int APP_COUNT=4;
const char* musicMenuItems[]={"SYNTH","TUNER","MUSIC PLAYER","WEB RADIO","EQUALIZER","MIC RECORDER"};
const char* systemMenuItems[]={"FILES","USB TRANSFER","WI-FI","UPDATE FIRMWARE","DEVICE STATUS","BLUETOOTH DEVICES","CLOCK / ALARMS","DISK MANAGER","PROCESS INFO","THEMES"};
const char* gamesMenuItems[]={"CONWAY LIFE"};
const char* hackMenuItems[]={"WI-FI RECON","BLE SCANNER","TCP CONNECT TEST","DNS LOOKUP","HTTP STATUS CHECK","NETWORK / RADIO INFO","OFFLINE VISUAL FX","HTTP SECURITY AUDIT","DEVICE SECURITY AUDIT"};
bool hackVisualFx=false;
uint32_t hackFxLastFrame=0;
constexpr int MUSIC_MENU_COUNT=6, SYSTEM_MENU_COUNT=10, GAMES_MENU_COUNT=1, HACK_MENU_COUNT=9;
String updateMessage="CONNECT TO WI-FI";
File micRecordFile;
bool micRecording=false;
uint32_t micRecordBytes=0,micRecordLastDraw=0;
String micRecordPath,micRecordStatus="READY";
int16_t micRecordSamples[3072];
const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
const float guitarHz[] = {82.41f,110.00f,146.83f,196.00f,246.94f,329.63f};
const char* guitarNames[] = {"E2","A2","D3","G3","B3","E4"};
const float bassHz[] = {41.20f,55.00f,73.42f,98.00f};
const char* bassNames[] = {"E1","A1","D2","G2"};
const float ukeHz[] = {196.00f,261.63f,329.63f,392.00f};
const char* ukeNames[] = {"G4","C4","E4","A4"};


}

void refreshBatteryCache(bool force=false) {
  if(!force && batteryCacheReady && millis()-batteryLastSample<30000UL) return;
  bool chargingNow=M5Cardputer.Power.isCharging()==m5::Power_Class::is_charging;
  int level=M5Cardputer.Power.getBatteryLevel();
  int16_t mv=M5Cardputer.Power.getBatteryVoltage();
  if(level>=0) {
    level=constrain(level,0,100);
    if(!chargingNow && batteryCacheReady && batteryCachedPercent>=0) level=min(level,batteryCachedPercent);
    batteryCachedPercent=level;
  }
  if(mv>0) batteryCachedMillivolts=mv;
  batteryCachedCharging=chargingNow;
  batteryLastSample=millis();
  batteryCacheReady=true;
}
int batteryPercent() { refreshBatteryCache(); return batteryCachedPercent; }
int16_t batteryMillivolts() { refreshBatteryCache(); return batteryCachedMillivolts; }
bool batteryCharging() { refreshBatteryCache(); return batteryCachedCharging; }
void drawBatteryIndicator() {
  auto &d=M5Cardputer.Display;
  int level=batteryPercent();
  bool charging=batteryCharging();
  d.fillRect(194,2,36,15,BG);
  d.drawRoundRect(207,4,19,10,2,level>=0 && level<=15?UI_PINK:UI_LAVENDER);
  d.fillRect(226,7,2,4,UI_LAVENDER);
  if(level>=0) { int width=constrain(level,0,100)*13/100; if(width>0)d.fillRect(210,6,width,6,level<=15?UI_PINK:UI_LAVENDER); }
  if(charging) { d.drawLine(202,3,198,9,UI_PINK); d.drawLine(198,9,201,9,UI_PINK); d.drawLine(201,9,197,15,UI_PINK); }
  if(WiFi.status()==WL_CONNECTED) { d.drawArc(184,10,5,5,205,335,UI_LAVENDER); d.drawArc(184,10,3,3,205,335,UI_LAVENDER); d.fillCircle(184,13,1,UI_LAVENDER); }
  else { d.drawLine(180,5,188,13,UI_MUTED); d.drawLine(188,5,180,13,UI_MUTED); }
}
void drawUiGrid(int top=20,int bottom=117){
  auto& d=M5Cardputer.Display;
  for(int x=15;x<240;x+=24)d.drawFastVLine(x,top,bottom-top,UI_GRID);
  for(int y=top+18;y<bottom;y+=20)d.drawFastHLine(7,y,226,UI_GRID);
  d.drawFastHLine(7,top,226,UI_EDGE);
}
void header(const char* title) {
  auto &d=M5Cardputer.Display;
  d.fillScreen(BG);d.fillRect(0,0,240,19,0x1808);drawUiGrid(20,117);
  d.fillRoundRect(6,3,11,11,3,0x3815);d.drawRoundRect(6,3,11,11,3,UI_PURPLE);
  d.drawFastVLine(11,5,7,UI_LAVENDER);d.drawFastHLine(8,8,7,UI_PINK);
  d.setTextSize(1);d.setTextColor(UI_PINK,BG);d.setCursor(21,5);d.print("JOHNNY");
  d.setTextColor(UI_MUTED,BG);d.setCursor(66,5);d.print("CARDPUTER ADV");drawBatteryIndicator();
  d.fillRoundRect(7,23,226,24,5,0x1C0D);d.drawRoundRect(7,23,226,24,5,UI_EDGE);
  d.fillRoundRect(7,23,3,24,1,UI_PINK);
  d.setTextSize(2);d.setTextColor(WHITE,0x1C0D);d.setCursor(15,28);d.print(title);d.setTextSize(1);
}
void footer(const char* s="") {
  auto& d=M5Cardputer.Display;
  d.drawFastHLine(7,117,226,0x5140);
  d.setTextSize(1);d.setTextColor(UI_MUTED,BG);d.setCursor(8,123);d.print(s);
}
void drawAppIcon(uint8_t app,int x,int y,uint16_t color){
  auto& d=M5Cardputer.Display;
  if(app==0){d.drawCircle(x+9,y+8,6,color);d.drawCircle(x+9,y+8,2,color);d.drawLine(x+13,y+12,x+18,y+17,color);d.drawFastHLine(x+3,y+8,3,color);}
  else if(app==1){d.drawRoundRect(x+1,y+4,18,12,4,color);d.drawFastHLine(x+4,y+10,6,color);d.drawFastVLine(x+7,y+7,6,color);d.fillCircle(x+14,y+8,1,color);d.fillCircle(x+17,y+12,1,color);}
  else if(app==2){d.fillCircle(x+5,y+14,3,color);d.fillCircle(x+15,y+11,3,color);d.drawFastVLine(x+8,y+2,12,color);d.drawFastVLine(x+18,y+1,10,color);d.drawFastHLine(x+8,y+2,10,color);}
  else{d.drawCircle(x+10,y+10,6,color);d.drawCircle(x+10,y+10,2,color);d.drawFastHLine(x,y+10,3,color);d.drawFastHLine(x+17,y+10,3,color);d.drawFastVLine(x+10,y,3,color);d.drawFastVLine(x+10,y+17,3,color);}
}
void drawHome(){
  auto& d=M5Cardputer.Display;d.fillScreen(BG);d.fillRect(0,0,240,19,0x1808);drawUiGrid(20,117);
  d.fillRoundRect(6,3,11,11,3,0x3815);d.drawRoundRect(6,3,11,11,3,UI_PURPLE);d.drawFastVLine(11,5,7,UI_LAVENDER);d.drawFastHLine(8,8,7,UI_PINK);
  d.setTextSize(1);d.setTextColor(UI_PINK,BG);d.setCursor(21,5);d.print("JOHNNY");d.setTextColor(UI_MUTED,BG);d.setCursor(66,5);d.print("CARDPUTER ADV // 1.0");drawBatteryIndicator();
  const char* labels[APP_COUNT]={"HACK / SECURITY","GAMES / ARCADE","MUSIC / STUDIO","SYSTEM / DEVICE"};
  const char* hints[APP_COUNT]={"WI-FI / BLE / NETWORK TOOLS","LIFE AND LIGHTWEIGHT GAMES","SYNTH / PLAYER / RADIO / REC","CLOCK / STORAGE / TASKS"};
  int idx=constrain(selected,0,APP_COUNT-1);
  d.fillRoundRect(8,29,224,84,7,UI_PANEL_HI);d.drawRoundRect(8,29,224,84,7,UI_PINK);d.fillRoundRect(8,29,5,84,2,UI_PINK);
  d.setTextSize(2);d.setTextColor(UI_CYAN,BG);d.setCursor(18,39);d.printf("0%d",idx+1);d.setTextSize(1);d.setTextColor(UI_LAVENDER,BG);d.setCursor(184,43);d.printf("%02d / 04",idx+1);
  d.fillRoundRect(18,60,38,38,7,0x3015);drawAppIcon((uint8_t)idx,27,69,UI_LAVENDER);
  d.setTextSize(2);d.setTextColor(WHITE,BG);d.setCursor(65,64);d.print(labels[idx]);d.setTextSize(1);d.setTextColor(UI_MUTED,BG);d.setCursor(65,88);d.print(hints[idx]);
  for(int i=0;i<APP_COUNT;i++)d.fillCircle(102+i*13,105,3,i==idx?UI_PINK:UI_EDGE);
  d.setTextColor(UI_MUTED,BG);d.setCursor(12,122);d.print("LEFT / RIGHT CHANGE SECTION   ENTER OPEN");
}
void drawModuleIcon(uint8_t category,uint8_t item,int x,int y,uint16_t color){
  auto& d=M5Cardputer.Display;
  if(category==2){
    switch(item){
      case 0:d.drawFastVLine(x+3,y+3,8,color);d.drawFastVLine(x+7,y+1,10,color);d.drawFastVLine(x+11,y+5,6,color);d.drawFastHLine(x+2,y+10,11,color);break;
      case 1:d.drawFastVLine(x+7,y+1,10,color);d.drawLine(x+7,y+2,x+3,y+7,color);d.drawLine(x+7,y+2,x+11,y+7,color);d.drawFastHLine(x+3,y+7,8,color);d.fillCircle(x+7,y+11,2,color);break;
      case 2:d.fillTriangle(x+3,y+1,x+3,y+12,x+12,y+6,color);break;
      case 3:d.drawFastVLine(x+7,y+6,6,color);d.drawFastHLine(x+4,y+12,7,color);d.drawArc(x+7,y+6,5,5,205,335,color);d.drawArc(x+7,y+6,9,9,205,335,color);break;
      case 4:d.drawFastVLine(x+2,y+7,5,color);d.drawFastVLine(x+5,y+3,9,color);d.drawFastVLine(x+8,y+1,11,color);d.drawFastVLine(x+11,y+5,7,color);break;
      default:d.drawRoundRect(x+3,y+1,8,10,3,color);d.drawFastVLine(x+7,y+11,3,color);d.drawFastHLine(x+4,y+14,7,color);break;
    }
  }else if(category==3){
    switch(item){
      case 0:d.drawRoundRect(x+1,y+4,13,9,2,color);d.drawLine(x+1,y+4,x+5,y+1,color);d.drawFastHLine(x+5,y+1,7,color);break;
      case 1:d.drawFastVLine(x+7,y+1,12,color);d.drawFastHLine(x+3,y+4,8,color);d.drawLine(x+3,y+4,x+5,y+2,color);d.drawLine(x+3,y+4,x+5,y+6,color);d.drawLine(x+11,y+9,x+9,y+7,color);d.drawLine(x+11,y+9,x+9,y+11,color);break;
      case 2:d.drawArc(x+7,y+9,5,5,205,335,color);d.drawArc(x+7,y+9,9,9,205,335,color);d.fillCircle(x+7,y+12,1,color);break;
      case 3:d.drawArc(x+7,y+7,6,6,40,320,color);d.drawLine(x+11,y+2,x+13,y+5,color);d.drawLine(x+11,y+2,x+8,y+3,color);break;
      case 4:d.drawRoundRect(x+2,y+1,11,12,2,color);d.drawFastHLine(x+4,y+4,7,color);d.drawFastHLine(x+4,y+7,7,color);d.drawFastHLine(x+4,y+10,4,color);break;
      default:d.drawLine(x+7,y+1,x+3,y+4,color);d.drawLine(x+7,y+1,x+11,y+4,color);d.drawLine(x+3,y+4,x+11,y+10,color);d.drawLine(x+11,y+4,x+3,y+10,color);d.drawLine(x+3,y+10,x+7,y+13,color);d.drawLine(x+11,y+10,x+7,y+13,color);break;
    }
  }else if(category==1){
    d.drawRoundRect(x+1,y+1,12,12,2,color);d.drawFastVLine(x+5,y+2,10,color);d.drawFastVLine(x+9,y+2,10,color);d.drawFastHLine(x+2,y+5,10,color);d.drawFastHLine(x+2,y+9,10,color);
  }else{
    switch(item){
      case 0:d.drawArc(x+7,y+7,5,5,205,335,color);d.drawArc(x+7,y+7,9,9,205,335,color);d.fillCircle(x+7,y+10,1,color);break;
      case 1:d.drawLine(x+7,y+1,x+3,y+4,color);d.drawLine(x+7,y+1,x+11,y+4,color);d.drawLine(x+3,y+4,x+11,y+10,color);d.drawLine(x+11,y+4,x+3,y+10,color);d.drawLine(x+3,y+10,x+7,y+13,color);d.drawLine(x+11,y+10,x+7,y+13,color);break;
      case 2:d.fillCircle(x+3,y+3,2,color);d.fillCircle(x+11,y+3,2,color);d.drawLine(x+3,y+3,x+7,y+7,color);d.drawLine(x+11,y+3,x+7,y+7,color);d.fillCircle(x+7,y+7,2,color);break;
      case 3:d.drawCircle(x+7,y+7,6,color);d.drawFastVLine(x+7,y+1,12,color);d.drawArc(x+7,y+7,4,6,90,270,color);d.drawArc(x+7,y+7,4,6,270,90,color);break;
      case 4:d.drawRoundRect(x+1,y+2,12,10,2,color);d.drawFastHLine(x+4,y+5,6,color);d.drawFastHLine(x+4,y+8,4,color);break;
      case 5:d.fillCircle(x+3,y+3,2,color);d.fillCircle(x+11,y+3,2,color);d.fillCircle(x+7,y+11,2,color);d.drawLine(x+4,y+4,x+6,y+9,color);d.drawLine(x+10,y+4,x+8,y+9,color);break;
      case 6:d.drawLine(x+7,y+1,x+9,y+5,color);d.drawLine(x+9,y+5,x+13,y+7,color);d.drawLine(x+13,y+7,x+8,y+8,color);d.drawLine(x+8,y+8,x+6,y+13,color);d.drawLine(x+6,y+13,x+5,y+8,color);d.drawLine(x+5,y+8,x+1,y+6,color);break;
      case 7:d.drawLine(x+7,y+1,x+12,y+3,color);d.drawLine(x+12,y+3,x+11,y+9,color);d.drawLine(x+11,y+9,x+7,y+13,color);d.drawLine(x+7,y+13,x+3,y+9,color);d.drawLine(x+3,y+9,x+2,y+3,color);d.drawLine(x+2,y+3,x+7,y+1,color);d.drawFastHLine(x+4,y+7,6,color);break;
      default:d.drawRoundRect(x+1,y+1,12,12,2,color);d.drawFastHLine(x+4,y+4,6,color);d.drawFastHLine(x+4,y+7,6,color);d.drawFastHLine(x+4,y+10,6,color);break;
    }
  }
}
void drawCategoryMenu(){
  const bool music=screen==MUSICMENU,system=screen==SYSTEMMENU,games=screen==GAMESMENU;
  const char* title=music?"MUSIC":(system?"SYSTEM":(games?"GAMES":"HACK"));
  const char** items=music?musicMenuItems:(system?systemMenuItems:(games?gamesMenuItems:hackMenuItems));
  const int count=music?MUSIC_MENU_COUNT:(system?SYSTEM_MENU_COUNT:(games?GAMES_MENU_COUNT:HACK_MENU_COUNT));
  const uint8_t category=music?2:(system?3:(games?1:0));
  const int visible=6;
  if(selected<0)selected=0;if(selected>=count)selected=count-1;
  if(selected<categoryScroll || selected>=categoryScroll+visible)categoryScroll=(selected/visible)*visible;
  header(title);auto& d=M5Cardputer.Display;
  d.fillRoundRect(8,51,224,12,3,0x2010);
  d.setTextColor(UI_MUTED,0x2010);d.setCursor(13,54);d.printf("%02d AVAILABLE MODULES",count);
  d.setTextColor(UI_CYAN,0x2010);d.setCursor(193,54);d.printf("%02d/%02d",selected+1,count);
  for(int cell=0;cell<visible;cell++){
    const int i=categoryScroll+cell;if(i>=count)break;
    const int x=8+(cell%2)*114,y=64+(cell/2)*17;
    const bool active=i==selected;const uint16_t fill=active?UI_PANEL_HI:0x1C0D;
    d.fillRoundRect(x,y,108,15,4,fill);d.drawRoundRect(x,y,108,15,4,active?UI_PINK:UI_EDGE);
    d.fillRoundRect(x+4,y+1,14,13,3,active?0x5020:0x2815);
    drawModuleIcon(category,(uint8_t)i,x+4,y+1,active?UI_LAVENDER:UI_MUTED);
    d.setTextColor(active?WHITE:UI_LAVENDER,fill);d.setCursor(x+22,y+4);
    String label=String(items[i]);if(label.length()>12)label=label.substring(0,11)+"~";
    d.print(label);if(active)d.fillCircle(x+101,y+7,2,UI_PINK);
  }
  footer(", / LEFT RIGHT  ; . UP DOWN  ENTER LAUNCH");
}
void drawDeviceStatus(){
  header("DEVICE STATUS");auto& d=M5Cardputer.Display;
  const bool connected=WiFi.status()==WL_CONNECTED;
  const String ip=connected?WiFi.localIP().toString():String("NO IP");
  String ssid=connected?WiFi.SSID():String("OFFLINE");if(ssid.length()>20)ssid=ssid.substring(0,18)+"..";
  d.fillRoundRect(8,52,224,29,5,0x2412);d.drawRoundRect(8,52,224,29,5,connected?UI_CYAN:UI_EDGE);
  d.fillCircle(19,65,3,connected?UI_CYAN:UI_PINK);
  d.setTextColor(connected?UI_CYAN:UI_PINK,0x2412);d.setCursor(29,57);d.print(connected?"NETWORK ONLINE":"NETWORK OFFLINE");
  d.setTextColor(WHITE,0x2412);d.setCursor(29,68);d.printf("%s / %s",ssid.c_str(),ip.c_str());
  d.fillRoundRect(8,86,108,26,4,0x2010);d.drawRoundRect(8,86,108,26,4,UI_EDGE);
  d.setTextColor(UI_MUTED,0x2010);d.setCursor(14,90);d.print("BATTERY");
  d.setTextColor(WHITE,0x2010);d.setCursor(14,101);d.printf("%d%% %s",batteryPercent(),batteryCharging()?"CHARGING":"");
  d.fillRoundRect(124,86,108,26,4,0x2010);d.drawRoundRect(124,86,108,26,4,UI_EDGE);
  d.setTextColor(UI_MUTED,0x2010);d.setCursor(130,90);d.print("HARDWARE");
  d.setTextColor(WHITE,0x2010);d.setCursor(130,101);d.printf("SD %s %uMHz",sdReady?"OK":"--",ESP.getCpuFreqMHz());
  footer("ESC BACK");
}
void drawUpdateInfo(){
  header("FIRMWARE UPDATE");auto& d=M5Cardputer.Display;d.setTextColor(UI_LAVENDER,BG);
  d.setCursor(8,47);d.print("SOURCE: GITHUB RELEASES");d.setTextColor(UI_MUTED,BG);
  d.setCursor(8,64);d.print(WiFi.status()==WL_CONNECTED?"WI-FI CONNECTED":"WI-FI OFFLINE");
  d.setCursor(8,82);d.print(updateMessage.substring(0,34));d.setCursor(8,94);d.print(updateMessage.substring(34,68));
  footer("ENTER INSTALL LATEST  ESC BACK");
}
void runFirmwareUpdate(){
  if(WiFi.status()!=WL_CONNECTED){updateMessage="CONNECT WI-FI FIRST";drawUpdateInfo();return;}
  updateMessage="DOWNLOADING LATEST RELEASE";drawUpdateInfo();
  WiFiClientSecure client;client.setInsecure();HTTPUpdate updater;updater.rebootOnUpdate(true);
  t_httpUpdate_return result=updater.update(client,"https://github.com/otavio-dias-leite/johnny/releases/latest/download/firmware.bin");
  if(result==HTTP_UPDATE_NO_UPDATES)updateMessage="NO UPDATE AVAILABLE";
  else if(result==HTTP_UPDATE_FAILED)updateMessage=String("UPDATE FAILED: ")+updater.getLastErrorString();
  else updateMessage="UPDATE INSTALLED - REBOOTING";
  drawUpdateInfo();
}

void splash(){
  auto &d=M5Cardputer.Display;
  d.setBrightness(190);d.fillScreen(BG);
  d.drawRoundRect(5,5,230,125,9,UI_EDGE);d.drawRoundRect(8,8,224,119,7,0x2815);
  for(int x=16;x<232;x+=18)d.drawFastVLine(x,14,105,UI_GRID);
  d.drawCircle(120,50,43,0x3014);d.drawCircle(120,50,37,0x4018);
  d.drawFastHLine(22,91,196,UI_PURPLE);d.drawFastHLine(34,94,172,UI_GRID);
  d.setTextSize(3);d.setTextColor(UI_PURPLE,BG);d.setCursor(65,37);d.print("JOHNNY");
  d.setTextColor(UI_LAVENDER,BG);d.setCursor(63,36);d.print("JOHNNY");
  d.drawFastHLine(57,67,126,UI_PINK);d.drawFastHLine(81,70,78,UI_CYAN);
  d.setTextSize(1);d.setTextColor(UI_PINK,BG);d.setCursor(72,82);d.print("WAKE FUCK UP SAMURAI");
  d.setTextColor(UI_MUTED,BG);d.setCursor(91,105);d.print("SYSTEM ONLINE");
  delay(1150);
}
void playBootJingle(){
  // Original JOHNNY startup motif; deliberately not a transcription of any game soundtrack.
  const uint16_t notes[8]={659,784,988,880,698,587,784,1047};
  for(size_t i=0;i<BOOT_JINGLE_SAMPLES;i++){
    size_t step=i/1024,offset=i%1024;float phase=6.28318530718f*(float)notes[step]*(float)offset/16000.0f;
    float attack=min(1.0f,(float)offset/90.0f),release=min(1.0f,(float)(1023-offset)/130.0f);
    float env=min(attack,release);float sample=(sinf(phase)+0.12f*sinf(phase*2.0f))*env*5200.0f;
    if(sample>32767.0f)sample=32767.0f;if(sample< -32768.0f)sample=-32768.0f;bootJingleBuffer[i]=(int16_t)sample;
  }
  M5Cardputer.Speaker.setVolume(118);
  M5Cardputer.Speaker.playRaw(bootJingleBuffer,BOOT_JINGLE_SAMPLES,16000,false,1,0,false);
}
void pianoScreen() {
  header("POCKET SYNTH");
  auto &d=M5Cardputer.Display;
  d.setTextColor(UI_MUTED,BG);d.setCursor(8,42);d.printf("OCT %d  %s  VOL %d%%",4+pianoOctave,effectNames[pianoEffect],pianoVolume);
  const char* labels[]={"C","D","E","F","G","A","B"};
  for(int i=0;i<7;i++){int x=5+i*33;uint16_t key=(i==1||i==3)?0xBDBF:0xEFFF;d.fillRoundRect(x,55,30,28,3,key);d.setTextColor(BG,key);d.setCursor(x+11,60);d.print(labels[i]);d.setCursor(x+10,71);d.printf("%c","QWERTYU"[i]);}
  d.setTextColor(UI_CYAN,BG);d.setCursor(8,88);d.print("1-7 SHARPS   S/A/L FLATS   Q-U / I-O-P NOTES");
  d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,101);d.printf("PADS Z X C V B N M  |  PAD %u %s",synthPadSelected+1,micRecording?"REC":"READY");
  d.setTextColor(UI_MUTED,BG);d.setCursor(8,113);d.print("R RECORD PAD   [ ] SELECT PAD");footer(", / OCTAVE   ; . TIMBRE   - / + VOLUME   ESC BACK");
}
float synthWave(uint32_t phase){return synthSineTable[(uint16_t)(phase>>22)];}
void requestSynthPad(uint8_t index){if(index>6||!sdReady)return;synthPadRequest=(int8_t)index;}
void playNote(char c){
  c=(char)tolower((unsigned char)c);
  const char* pads="zxcvbnm";const char* pad=strchr(pads,c);
  if(pad){requestSynthPad((uint8_t)(pad-pads));return;}
  int semitone=-1;const char* natural="qwertyuiop";const int offsets[10]={0,2,4,5,7,9,11,12,14,16};const char* n=strchr(natural,c);if(n)semitone=offsets[n-natural];
  const char* sharp="1234567";const int sharpOffsets[7]={1,3,6,8,10,13,15};const char* sh=strchr(sharp,c);if(sh)semitone=sharpOffsets[sh-sharp];
  if(c=='s')semitone=1;else if(c=='a')semitone=3;else if(c=='l')semitone=6;
  if(semitone<0||pianoVolume<=0)return;
  if(!synthSineReady){for(size_t i=0;i<1024;i++)synthSineTable[i]=sinf(6.28318530718f*(float)i/1024.0f);synthSineReady=true;}
  if(M5Cardputer.Speaker.isPlaying(SYNTH_AUDIO_CHANNEL)>=2)return;
  if(audioMutex&&xSemaphoreTake(audioMutex,pdMS_TO_TICKS(4))!=pdTRUE)return;
  const int midi=60+pianoOctave*12+semitone;const float hz=440.0f*powf(2.0f,((float)midi-69.0f)/12.0f);
  const uint32_t phaseStep=(uint32_t)((double)hz*(4294967296.0/22050.0));const float gain=(float)pianoVolume*1.05f*256.0f;
  const uint8_t slot=synthAudioBufferIndex;int16_t* samples=synthAudioBuffers[slot];uint32_t phase=0;
  for(size_t i=0;i<SYNTH_SAMPLES;i++){
    float a=synthWave(phase),value=a;
    if(pianoEffect==1)value=0.78f*a+0.18f*synthWave(phase*2U);
    else if(pianoEffect==2)value=(a*2.2f)/(1.0f+fabsf(a*2.2f));
    else if(pianoEffect==3)value=(a*1.8f+0.6f*synthWave(phase*2U))/(1.0f+fabsf(a*1.8f));
    else if(pianoEffect==4)value=a>=0.0f?0.72f:-0.72f;
    else if(pianoEffect==5)value=0.7f*a+0.25f*synthWave(phase*2U);
    else if(pianoEffect==6)value=0.78f*a+0.18f*synthWave(phase*2U)+0.08f*synthWave(phase*3U);
    else if(pianoEffect==7)value=0.8f*a+0.2f*synthWave(phase/2U);
    float env=1.0f;if(i<100)env=(float)i/100.0f;else if(i>SYNTH_SAMPLES-500)env=(float)(SYNTH_SAMPLES-i)/500.0f;
    float out=value*env*gain;if(out>32767.0f)out=32767.0f;if(out< -32768.0f)out=-32768.0f;samples[i]=(int16_t)out;phase+=phaseStep;
  }
  if(M5Cardputer.Speaker.playRaw(samples,SYNTH_SAMPLES,22050,false,1,SYNTH_AUDIO_CHANNEL,false))synthAudioBufferIndex=(uint8_t)((slot+1)%3);
  if(audioMutex)xSemaphoreGive(audioMutex);
}
void stopMicCapture() {
  if(micStarted) M5Cardputer.Mic.end();
  micStarted=false; micWarmupUntil=0;
}
bool captureMic(int16_t* dst, size_t count, uint32_t timeoutMs=260) {
  if(!micStarted) {
    if(!M5Cardputer.Mic.begin()) return false;
    micStarted=true; micWarmupUntil=millis()+1000;
  }
  if((int32_t)(millis()-micWarmupUntil)<0) return false;
  if(!M5Cardputer.Mic.record(dst,count,16000,false)) { stopMicCapture(); return false; }
  uint32_t started=millis();
  while(M5Cardputer.Mic.isRecording() && millis()-started<timeoutMs) delay(1);
  bool complete=!M5Cardputer.Mic.isRecording();
  if(!complete) stopMicCapture();
  return complete;}
float detectPitch(const int16_t* samples,size_t count,float* rmsOut=nullptr){
  if(count<1200)return 0.0f;
  double mean=0.0;for(size_t i=0;i<count;i++)mean+=(double)samples[i];mean/=(double)count;
  double energy=0.0;for(size_t i=0;i<count;i++){double v=(double)samples[i]-mean;energy+=v*v;}
  const float rms=sqrtf((float)(energy/(double)count));if(rmsOut)*rmsOut=rms;if(rms<130.0f)return 0.0f;
  // YIN difference/CMND estimates the fundamental rather than the loudest harmonic.
  const int minLag=28,maxLag=min(430,(int)count/2);const size_t overlap=count-(size_t)maxLag;
  static float yin[432];
  for(int tau=1;tau<=maxLag;tau++){float sum=0.0f;for(size_t i=0;i<overlap;i++){const float x=(float)samples[i]-(float)mean,y=(float)samples[i+(size_t)tau]-(float)mean,delta=x-y;sum+=delta*delta;}yin[tau]=sum;}
  float running=0.0f;for(int tau=1;tau<=maxLag;tau++){running+=yin[tau];yin[tau]=(running>0.0f)?yin[tau]*(float)tau/running:1.0f;}
  int best=-1;
  for(int tau=minLag;tau<=maxLag;tau++)if(yin[tau]<0.18f){while(tau<maxLag&&yin[tau+1]<yin[tau])tau++;best=tau;break;}
  if(best<0){float lowest=1.0f;for(int tau=minLag;tau<=maxLag;tau++)if(yin[tau]<lowest){lowest=yin[tau];best=tau;}if(best<0||lowest>0.30f)return 0.0f;}
  float refined=(float)best;
  if(best>minLag&&best<maxLag){const float l=yin[best-1],c=yin[best],r=yin[best+1],denom=l-2.0f*c+r;if(fabsf(denom)>0.000001f)refined+=0.5f*(l-r)/denom;}
  if(refined<=0.0f)return 0.0f;const float frequency=16000.0f/refined;return(frequency>=36.0f&&frequency<=560.0f)?frequency:0.0f;
}
void drawTunerReadout(){
  auto& d=M5Cardputer.Display;d.fillRect(7,51,226,67,BG);
  const float* hzTable=(tunerInstrument==1)?bassHz:((tunerInstrument==2)?ukeHz:guitarHz);
  const char** nameTable=(tunerInstrument==1)?bassNames:((tunerInstrument==2)?ukeNames:guitarNames);
  const int count=(tunerInstrument==1||tunerInstrument==2)?4:6;
  int nearest=-1;float cents=0.0f;
  if(tunerHasSignal&&tunerFrequency>0.0f){float bestDistance=1.0e9f;for(int i=0;i<count;i++){const float delta=1200.0f*log2f(tunerFrequency/hzTable[i]);if(fabsf(delta)<bestDistance){bestDistance=fabsf(delta);cents=delta;nearest=i;}}}
  const int cellWidth=(tunerInstrument==0)?36:54,gap=(tunerInstrument==0)?1:2;
  for(int i=0;i<count;i++){const int x=8+i*(cellWidth+gap);const bool active=i==nearest;const uint16_t fill=active?PANEL:BG;d.fillRoundRect(x,52,cellWidth,11,2,fill);if(active)d.drawRoundRect(x,52,cellWidth,11,2,UI_PINK);d.setTextColor(active?WHITE:UI_MUTED,fill);d.setCursor(x+3,54);d.print(nameTable[i]);}
  d.fillRoundRect(8,67,224,43,4,PANEL);
  if(nearest<0){d.setTextColor(UI_LAVENDER,PANEL);d.setCursor(17,80);d.setTextSize(2);d.print("PLAY A STRING");d.setTextSize(1);d.setTextColor(UI_MUTED,PANEL);d.setCursor(17,99);d.print("WAITING FOR STABLE PITCH");}
  else{
    const uint16_t color=fabsf(cents)<=5.0f?UI_LAVENDER:UI_PINK;
    d.setTextColor(UI_MUTED,PANEL);d.setCursor(16,72);d.print("NEAREST OPEN STRING");
    d.setTextSize(3);d.setTextColor(WHITE,PANEL);d.setCursor(16,81);d.print(nameTable[nearest]);d.setTextSize(1);
    d.setTextColor(UI_LAVENDER,PANEL);d.setCursor(91,84);d.printf("TARGET %.1f",hzTable[nearest]);
    d.setTextColor(color,PANEL);d.setCursor(91,97);if(fabsf(cents)<=5.0f)d.print("IN TUNE");else d.printf("%+.0f CENT %s",cents,cents<0.0f?"TUNE UP":"TUNE DOWN");
    d.setTextColor(UI_MUTED,BG);d.setCursor(9,114);d.printf("INPUT %.1f Hz",tunerFrequency);
  }
}

void tunerScreen() {
  header("TUNER");
  auto &d=M5Cardputer.Display;
  const char* modeNames[]={"GUITAR","BASS","UKULELE"};
  d.setTextColor(UI_PINK,BG); d.setCursor(8,43); d.print(modeNames[tunerInstrument]);
  d.setTextColor(UI_MUTED,BG); d.setCursor(166,43); d.print("<  >");
  drawTunerReadout();
  footer("");
}
void drawNetworkScreen() {
  header("WI-FI");
  auto &d=M5Cardputer.Display;
  if(wifiConfigStage==1) {
    if(wifiScanOnly&&wifiSurveyDetails&&wifiSelected>=0&&wifiSelected<wifiNetworkCount){
      d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,43);d.print("NETWORK DETAILS");
      String name=wifiNetworks[wifiSelected];if(name.length()>26)name=name.substring(0,23)+"...";
      d.setTextColor(WHITE,BG);d.setCursor(8,59);d.printf("SSID: %s",name.c_str());
      d.setCursor(8,73);d.printf("BSSID: %.17s",wifiNetworkBssid[wifiSelected].c_str());
      d.setCursor(8,87);d.printf("RSSI: %d dBm  CHANNEL: %d",wifiNetworkRSSI[wifiSelected],wifiNetworkChannel[wifiSelected]);
      d.setCursor(8,101);d.printf("SECURITY: %s",wifiNetworkAuth[wifiSelected]==WIFI_AUTH_OPEN?"OPEN":"PROTECTED");
      footer("ESC BACK TO SCAN  R RESCAN");return;
    }
    d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,43);
    d.print(wifiScanPending?"SCANNING NETWORKS...":(wifiScanOnly?"WI-FI SURVEY":"SELECT A NETWORK"));
    if(!wifiScanPending && wifiNetworkCount==0) {
      d.setTextColor(UI_MUTED,BG); d.setCursor(8,68); d.print("NO NETWORKS FOUND");
      d.setCursor(8,83); d.print("PRESS R TO SCAN AGAIN");
    }
    for(int row=0;row<5 && wifiScroll+row<wifiNetworkCount;row++) {
      int i=wifiScroll+row, y=57+row*12;
      bool active=i==wifiSelected;
      uint16_t bg=active?PANEL:BG;
      d.fillRect(7,y-1,225,11,bg);
      d.setTextColor(active?WHITE:UI_MUTED,bg); d.setCursor(10,y);
      String name=wifiNetworks[i];if(name.length()>12)name=name.substring(0,10)+"..";
      d.printf("%c %-12s %ddB C%02d",active?'>':' ',name.c_str(),wifiNetworkRSSI[i],wifiNetworkChannel[i]);
      d.setTextColor(wifiNetworkAuth[i]==WIFI_AUTH_OPEN?UI_LAVENDER:UI_MUTED,bg);d.setCursor(213,y);d.print(wifiNetworkAuth[i]==WIFI_AUTH_OPEN?"O":"L");
    }
    footer("UP/DN SELECT  ENTER JOIN  R SCAN");
    return;
  }
  if(wifiConfigStage==2) {
    d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,43); d.print("NETWORK");
    d.setTextColor(WHITE,BG); d.setCursor(8,57); d.print(savedSsid.substring(0,28));
    d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,76); d.print("PASSWORD");
    d.setTextColor(WHITE,BG); d.setCursor(8,91);
    String masked=""; for(size_t i=0;i<wifiEntry.length() && i<28;i++) masked+="*";
    d.print(masked);
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,105); d.print("TYPE PASSWORD + ENTER");
    footer("ESC BACK TO NETWORKS");
    return;
  }
  if(WiFi.status()==WL_CONNECTED) {
    d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,48); d.print("CONNECTED");
    d.setTextColor(WHITE,BG); d.setCursor(8,66); d.print(WiFi.SSID());
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,82); d.print(WiFi.localIP().toString());
    d.setCursor(8,98); d.printf("SIGNAL %d dBm",WiFi.RSSI());
  } else {
    d.setTextColor(UI_PINK,BG); d.setCursor(8,54); d.print("NOT CONNECTED");
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,75); d.print(savedSsid.length()?savedSsid:"NO NETWORK SAVED");
  }
  footer("ENTER SEARCH NETWORKS  ESC BACK");
}

uint8_t audioProgressPercent();
void drawBleScreen();
void stopAudio();
void stopBleScan();
void onBleScanComplete(BLEScanResults results){(void)results;bleScanDone=true;}
void startBleScan(){
  stopAudio();
  bleCount=0;bleScanDone=false;bleScanning=false;hackMessage="STARTING PASSIVE SCAN";
  if(!bleInitialized){BLEDevice::init("");bleInitialized=true;}
  if(!bleScanner)bleScanner=BLEDevice::getScan();
  if(!bleScanner){hackMessage="BLE INIT FAILED";return;}
  bleScanner->setActiveScan(false);bleScanner->setInterval(100);bleScanner->setWindow(60);
  bleScanning=bleScanner->start(5,onBleScanComplete,false);
  if(!bleScanning)hackMessage="BLE SCAN START FAILED";
}
void serviceBleScan(){
  if(!bleScanning||!bleScanDone||!bleScanner)return;
  bleScanDone=false;bleScanning=false;BLEScanResults results=bleScanner->getResults();
  bleCount=min(8,results.getCount());
  for(int i=0;i<bleCount;i++){
    BLEAdvertisedDevice device=results.getDevice((uint32_t)i);
    String name=device.haveName()?String(device.getName().c_str()):String("UNKNOWN");
    String address=String(device.getAddress().toString().c_str());
    if(name.length()>12)name=name.substring(0,12);
    bleNames[i]=name+" "+address.substring(max(0,(int)address.length()-5));
    bleRssi[i]=device.haveRSSI()?device.getRSSI():-127;
  }
  hackMessage=bleCount?String(bleCount)+" DEVICES FOUND":"NO BLE DEVICES FOUND";
  bleScanner->clearResults();if(screen==BLESCAN&&!screenSaverActive)drawBleScreen();
}
void stopBleScan(){
  if(bleScanner){bleScanner->stop();bleScanner->clearResults();}
  bleScanner=nullptr;bleScanning=false;bleScanDone=false;
  if(bleInitialized){BLEDevice::deinit(true);bleInitialized=false;}
}
void drawBleScreen(){
  header("BLE SCANNER");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,42);d.print(bleScanning?"PASSIVE SCAN IN PROGRESS":hackMessage.substring(0,28));
  for(int i=0;i<min(5,bleCount);i++){int y=57+i*11;d.setTextColor(WHITE,BG);d.setCursor(9,y);d.print(bleNames[i].substring(0,20));d.setTextColor(UI_MUTED,BG);d.setCursor(194,y);d.printf("%d",bleRssi[i]);}
  footer("R RESCAN  ESC BACK");
}

void drawHackInput(){
  header(hackInputMode==0?"TCP CONNECT TEST":(hackInputMode==1?"DNS LOOKUP":(hackInputMode==2?"HTTP STATUS CHECK":"HTTP SECURITY AUDIT")));
  auto& d=M5Cardputer.Display;
  const char* hint=hackInputMode==0?"IP:PORT (e.g. 192.168.1.1:80)":(hackInputMode==1?"HOSTNAME (e.g. router.local)":(hackInputMode==2?"HTTP URL (http://192.168.1.1)":"HTTP URL (http://example.local)"));
  d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,44);d.print(hint);
  d.fillRoundRect(7,62,225,21,4,PANEL);d.setTextColor(WHITE,PANEL);d.setCursor(12,69);
  String value=hackInputValue;if(value.length()>30)value="..."+value.substring(value.length()-27);d.print(value);
  d.setTextColor(hackInputDone?UI_PINK:UI_MUTED,BG);d.setCursor(8,94);
  if(hackInputDone){String result=hackResultMessage;if(result.length()>34)result=result.substring(0,31)+"...";d.print(result);d.setCursor(8,108);d.print("R NEW TEST");}
  else d.print(WiFi.status()==WL_CONNECTED?"ENTER TO RUN  ESC BACK":"CONNECT WI-FI FIRST");
  footer("TYPE TARGET  ENTER TEST  ESC BACK");
}
void runHackInput(){
  if(WiFi.status()!=WL_CONNECTED){hackResultMessage="WI-FI NOT CONNECTED";hackInputDone=true;drawHackInput();return;}
  if(hackInputMode==0){
    int split=hackInputValue.lastIndexOf(':');int port=split>0?hackInputValue.substring(split+1).toInt():0;
    String host=split>0?hackInputValue.substring(0,split):String("");IPAddress address;
    if(!address.fromString(host)||port<1||port>65535){hackResultMessage="USE IP:PORT";hackInputDone=true;drawHackInput();return;}
    WiFiClient client;bool ok=client.connect(address,(uint16_t)port,2500);client.stop();
    hackResultMessage=ok?"TCP OPEN / REACHABLE":"CLOSED / NO RESPONSE";
  }else if(hackInputMode==1){
    IPAddress address;hackResultMessage=WiFi.hostByName(hackInputValue.c_str(),address)?("DNS "+address.toString()):"DNS LOOKUP FAILED";
  }else if(hackInputMode==2){
    if(!hackInputValue.startsWith("http://")){hackResultMessage="USE HTTP:// URL";hackInputDone=true;drawHackInput();return;}
    HTTPClient http;http.setTimeout(2500);
    if(!http.begin(hackInputValue))hackResultMessage="INVALID HTTP URL";
    else{int code=http.sendRequest("HEAD");hackResultMessage=code>0?("HTTP STATUS "+String(code)):"HTTP REQUEST FAILED";http.end();}
  }else{
    if(!hackInputValue.startsWith("http://")){hackResultMessage="USE HTTP:// URL";hackInputDone=true;drawHackInput();return;}
    const char* keys[]={"Strict-Transport-Security","Content-Security-Policy","X-Frame-Options","X-Content-Type-Options","Referrer-Policy"};
    HTTPClient http;http.setTimeout(3000);http.collectHeaders(keys,5);
    if(!http.begin(hackInputValue))hackResultMessage="INVALID HTTP URL";
    else{
      int code=http.sendRequest("HEAD");
      if(code>0){
        String report="HTTP "+String(code)+" ";int present=0;
        if(http.header(keys[0]).length()){report+="HSTS ";present++;}
        if(http.header(keys[1]).length()){report+="CSP ";present++;}
        if(http.header(keys[2]).length()){report+="XFO ";present++;}
        if(http.header(keys[3]).length()){report+="NOSNIFF ";present++;}
        if(http.header(keys[4]).length()){report+="REF ";present++;}
        if(!present)report+="NO SECURITY HEADERS";
        hackResultMessage=report;
      }else hackResultMessage="HTTP REQUEST FAILED";
      http.end();
    }
  }
  hackInputDone=true;drawHackInput();
}

void drawHackInfo(){
  if(selected==8){
    header("DEVICE SECURITY AUDIT");auto& d=M5Cardputer.Display;
    d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,43);d.printf("FREE HEAP %lu",(unsigned long)ESP.getFreeHeap());
    d.setTextColor(WHITE,BG);d.setCursor(8,57);d.printf("MIN HEAP %lu",(unsigned long)ESP.getMinFreeHeap());
    d.setCursor(8,70);d.printf("FLASH %lu KB",(unsigned long)(ESP.getFlashChipSize()/1024UL));
    d.setCursor(8,83);d.printf("CPU %u MHz  PSRAM %lu",ESP.getCpuFreqMHz(),(unsigned long)ESP.getPsramSize());
    d.setCursor(8,96);d.printf("SD %s  WIFI %s",sdReady?"READY":"MISSING",WiFi.status()==WL_CONNECTED?"ON":"OFF");
    d.setTextColor(UI_MUTED,BG);d.setCursor(8,110);d.print("LOCAL INVENTORY / NO NETWORK TEST");footer("ESC BACK");return;
  }
  header("NETWORK / RADIO INFO");auto& d=M5Cardputer.Display;
  bool connected=WiFi.status()==WL_CONNECTED;
  d.setTextColor(connected?UI_LAVENDER:UI_PINK,BG);d.setCursor(8,43);d.print(connected?"STA CONNECTED":"STA OFFLINE");
  d.setTextColor(WHITE,BG);d.setCursor(8,57);d.printf("IP %s",connected?WiFi.localIP().toString().c_str():"0.0.0.0");
  d.setCursor(8,70);d.printf("MAC %.17s",WiFi.macAddress().c_str());
  d.setCursor(8,83);d.printf("RSSI %d dBm CH %d",connected?WiFi.RSSI():0,connected?WiFi.channel():0);
  d.setCursor(8,96);d.printf("GW %s",connected?WiFi.gatewayIP().toString().c_str():"N/A");
  d.setTextColor(UI_MUTED,BG);d.setCursor(8,110);d.print("PASSIVE TOOLS / NO RF FLOOD");footer("ESC BACK");
}
void drawHackFxScreen(){
  header("OFFLINE VISUAL FX");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_PINK,BG);d.setCursor(8,48);d.print("LOCAL DISPLAY EFFECT");
  d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,68);d.print("NO WI-FI / BLE TRANSMISSION");
  d.setCursor(8,84);d.print("NO EXTERNAL DEVICE AFFECTED");
  footer("ESC BACK");
}


String jsonEscape(const String& in){String out;out.reserve(in.length()+8);for(size_t i=0;i<in.length();i++){char c=in[i];if(c=='"'||c=='\\'){out+='\\';out+=c;}else if((uint8_t)c>=32)out+=c;}return out;}
void sendWebStatus(){
  bool connected=WiFi.status()==WL_CONNECTED;
  String json="{";
  json+="\"firmware\":\""+String(JOHNNY_VERSION)+"\",";
  json+="\"hostname\":\"johnny.local\",";
  json+="\"connected\":"+String(connected?"true":"false")+",";
  json+="\"ip\":\""+(connected?WiFi.localIP().toString():String("0.0.0.0"))+"\",";
  json+="\"mac\":\""+WiFi.macAddress()+"\",";
  json+="\"ssid\":\""+jsonEscape(connected?WiFi.SSID():String(""))+"\",";
  json+="\"rssi\":"+String(connected?WiFi.RSSI():0)+",";
  json+="\"gateway\":\""+(connected?WiFi.gatewayIP().toString():String("0.0.0.0"))+"\",";
  json+="\"subnet\":\""+(connected?WiFi.subnetMask().toString():String("0.0.0.0"))+"\",";
  json+="\"dns\":\""+(connected?WiFi.dnsIP().toString():String("0.0.0.0"))+"\",";
  json+="\"sd_ready\":"+String(sdReady?"true":"false")+",";
  json+="\"battery_percent\":"+String(batteryPercent())+",";
  json+="\"battery_mv\":"+String(batteryMillivolts())+",";
  json+="\"charging\":"+String(batteryCharging()?"true":"false")+",";
  json+="\"free_heap\":"+String(ESP.getFreeHeap())+",";
  json+="\"min_free_heap\":"+String(ESP.getMinFreeHeap())+",";
  json+="\"psram_bytes\":"+String(ESP.getPsramSize())+",";
  json+="\"cpu_mhz\":"+String(ESP.getCpuFreqMHz())+",";
  json+="\"uptime_seconds\":"+String(millis()/1000UL)+"}";
  johnnyWeb.sendHeader("Cache-Control","no-store");
  johnnyWeb.send(200,"application/json; charset=utf-8",json);
}
static const char JOHNNY_WEB_HOME[] PROGMEM=R"JOHNNYWEB(
<!doctype html><html lang="pt-BR"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>JOHNNY // Status</title>
<style>
:root{color-scheme:dark;--bg:#10081f;--panel:#20132f;--line:#6c3c91;--pink:#f19fff;--text:#eee7f6;--muted:#b5b4c8}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px system-ui,sans-serif}
main{max-width:620px;margin:auto;padding:18px}header{border-bottom:1px solid var(--line);padding:8px 0 14px;display:flex;justify-content:space-between;gap:10px}
h1{font-size:20px;letter-spacing:.15em;margin:0}.state{color:var(--pink);font-size:11px}
#status{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;margin-top:14px}
.item{background:var(--panel);border:1px solid #49305f;border-radius:8px;padding:10px;min-width:0;overflow-wrap:anywhere}
.item b{display:block;color:var(--pink);font-size:10px;letter-spacing:.1em;margin-bottom:4px}
footer{margin-top:16px;color:var(--muted);font-size:11px}
</style></head><body><main><header><h1>JOHNNY</h1><span class="state">DEVICE STATUS</span></header>
<div id="status">Connecting…</div><footer>Atualização automática a cada 15 segundos. A tela e os controles permanecem no Cardputer.</footer></main>
<script>
const status=document.getElementById('status');
async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error();const j=await r.json();
const rows=[['FIRMWARE',j.firmware],['CONEXÃO',j.connected?'ONLINE':'OFFLINE'],['IP',j.ip],['WI-FI',j.ssid||'OFFLINE'],['SINAL',j.rssi+' dBm'],['BATERIA',j.battery_percent+'% / '+j.battery_mv+' mV'],['CARGA',j.charging?'SIM':'NÃO'],['MICROSD',j.sd_ready?'MONTADO':'AUSENTE'],['RAM LIVRE',j.free_heap+' bytes'],['UPTIME',j.uptime_seconds+' s']];
status.replaceChildren(...rows.map(([k,v])=>{const el=document.createElement('div');el.className='item';const b=document.createElement('b');b.textContent=k;const span=document.createElement('span');span.textContent=String(v);el.append(b,span);return el;}));
}catch(e){status.textContent='Sem conexão com o Cardputer.'}}
refresh();setInterval(refresh,15000);
</script></body></html>
)JOHNNYWEB";
void beginJohnnyWeb(){
  johnnyWeb.on("/",HTTP_GET,[]{johnnyWeb.send_P(200,"text/html; charset=utf-8",JOHNNY_WEB_HOME);});
  johnnyWeb.on("/api/status",HTTP_GET,sendWebStatus);
  johnnyWeb.onNotFound([]{johnnyWeb.send(404,"text/plain","JOHNNY: endpoint not found");});
  johnnyWeb.begin();johnnyWebStarted=true;
}
void serviceWebServer(){
  bool connected=WiFi.status()==WL_CONNECTED;
  if(connected&&!johnnyWebStarted)beginJohnnyWeb();
  if(connected&&johnnyWebStarted)johnnyWeb.handleClient();
  if(connected&&!johnnyMdnsStarted){johnnyMdnsStarted=MDNS.begin("johnny");if(johnnyMdnsStarted)MDNS.addService("http","tcp",80);}
  if(!connected&&johnnyMdnsStarted){MDNS.end();johnnyMdnsStarted=false;}
}

void startNetworkSetup() {
  stopAudio();wifiScanOnly=false;wifiSurveyDetails=false;
  WiFi.mode(WIFI_STA); WiFi.disconnect(false,false);
  wifiNetworkCount=0; wifiSelected=0; wifiScroll=0; wifiScanPending=true;
  wifiConfigStage=1; WiFi.scanDelete(); WiFi.scanNetworks(true,false);
  drawNetworkScreen();
}
void startWifiSurvey() {
  stopAudio();wifiSurveyDetails=false;
  WiFi.mode(WIFI_STA);wifiNetworkCount=0;wifiSelected=0;wifiScroll=0;wifiScanPending=true;
  wifiConfigStage=1;wifiScanOnly=true;WiFi.scanDelete();WiFi.scanNetworks(true,false);drawNetworkScreen();
}
void saveNetworkSetup();
void chooseNetwork(int index) {
  if(index<0 || index>=wifiNetworkCount) return;
  savedSsid=wifiNetworks[index]; wifiEntry="";
  if(wifiNetworkAuth[index]==WIFI_AUTH_OPEN) { wifiConfigStage=2; saveNetworkSetup(); }
  else { wifiConfigStage=2; drawNetworkScreen(); }
}
void saveNetworkSetup() {
  if(wifiConfigStage!=2) return;
  savedPassword=wifiEntry;
  preferences.putString("ssid",savedSsid);
  preferences.putString("pass",savedPassword);
  wifiConfigStage=0; wifiScanPending=false;
  WiFi.disconnect(false,false);
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(savedSsid.c_str(),savedPassword.c_str());
  lastWiFiStatus=-1;
  drawNetworkScreen();
}
void stopAudio();
bool beginUsbMode();
void startBleScan();void drawBleScreen();void serviceBleScan();
void drawHackFxScreen();void drawHackInfo();
void moveMusicTrack(int direction);void playSelectedTrack();uint8_t audioProgressPercent();
void drawUsbScreen(bool ok);
String safeSdPath(String path) {
  path.trim();
  path.replace("\\","/");
  if(!path.startsWith("/")) path="/"+path;
  if(path.indexOf("..")>=0 || path.indexOf('\t')>=0 || path.indexOf('\n')>=0 || path.indexOf('\r')>=0) return "";
  return path;
}
void usbReplyError(const char* reason) {
  Serial.print("ERR\t"); Serial.println(reason);
}
void processUsbCommand(String line) {
  line.trim();
  if(line=="PING") { Serial.println("JOHNNY/2"); return; }
  if(line=="ENTER") {
    if(beginUsbMode()) { screen=USBMODE; drawUsbScreen(true); Serial.println("READY"); }
    else usbReplyError("SD");
    return;
  }
  if(line=="EXIT") {
    usbActive=false; screen=HOME; drawHome(); Serial.println("BYE"); return;
  }
  if(!usbActive) { usbReplyError("MODE"); return; }
  if(line.startsWith("LIST\t")) {
    String path=safeSdPath(line.substring(5));
    if(path.length()==0) { usbReplyError("PATH"); return; }
    File dir=SD.open(path);
    if(!dir || !dir.isDirectory()) { if(dir)dir.close(); usbReplyError("DIRECTORY"); return; }
    File entry=dir.openNextFile();
    while(entry) {
      String name=String(entry.name());
      int slash=name.lastIndexOf('/');
      if(slash>=0) name=name.substring(slash+1);
      if(name.length() && name!="System Volume Information") {
        if(entry.isDirectory()) { Serial.print("DIR\t"); Serial.println(name); }
        else { Serial.printf("FILE\t%lu\t",(unsigned long)entry.size()); Serial.println(name); }
      }
      entry.close(); entry=dir.openNextFile();
    }
    dir.close(); Serial.println("END"); return;
  }
  if(line.startsWith("GET\t")) {
    String path=safeSdPath(line.substring(4));
    if(path.length()==0) { usbReplyError("PATH"); return; }
    File file=SD.open(path,FILE_READ);
    if(!file || file.isDirectory()) { if(file)file.close(); usbReplyError("OPEN"); return; }
    size_t size=file.size();
    if(size>64UL*1024UL*1024UL){file.close();usbReplyError("LIMIT");return;}
    Serial.printf("DATA\t%lu\n",(unsigned long)size);
    uint8_t buffer[512];
    while(file.available()) {
      int n=file.read(buffer,sizeof(buffer));
      if(n<=0) break;
      Serial.write(buffer,(size_t)n);
    }
    file.close(); Serial.println("END"); return;
  }
  if(line.startsWith("PUT\t")) {
    int split=line.indexOf('\t',4);
    if(split<0) { usbReplyError("FORMAT"); return; }
    uint32_t size=(uint32_t)line.substring(4,split).toInt();
    String path=safeSdPath(line.substring(split+1));
    if(path.length()==0 || path=="/" || size>64UL*1024UL*1024UL) { usbReplyError("LIMIT"); return; }
    if(SD.exists(path)) SD.remove(path);
    File file=SD.open(path,FILE_WRITE);
    if(!file) { usbReplyError("CREATE"); return; }
    Serial.println("READY");
    uint32_t received=0,lastByte=millis();
    uint8_t buffer[512];
    while(received<size) {
      const size_t expected=(size_t)min((uint32_t)sizeof(buffer),size-received);
      size_t got=0;
      while(got<expected && millis()-lastByte<15000UL) {
        int available=Serial.available();
        if(available<=0) { delay(1); continue; }
        size_t ask=min((size_t)available,expected-got);
        size_t part=Serial.readBytes((char*)buffer+got,ask);
        if(part>0) { got+=part; lastByte=millis(); }
      }
      if(got!=expected) break;
      size_t written=file.write(buffer,expected);
      if(written!=expected) break;
      received+=(uint32_t)written;
      Serial.printf("ACK\t%lu\n",(unsigned long)received);
    }
    file.flush();file.close();
    if(received==size)Serial.println("OK");
    else { SD.remove(path);usbReplyError("INCOMPLETE"); }
    return;
  }
  if(line.startsWith("DELETE\t")) {
    String path=safeSdPath(line.substring(7));
    if(path.length()==0 || path=="/") { usbReplyError("PATH"); return; }
    if(SD.remove(path)) Serial.println("OK"); else usbReplyError("DELETE");
    return;
  }
  usbReplyError("COMMAND");
}
bool beginUsbMode() {
  if(!sdReady)return false;
  stopAudio();
  uint32_t started=millis();
  while(radioRecordState==3&&millis()-started<5000UL)delay(5);
  if(radioRecordState==3)return false;
  usbCommandLine="";
  usbActive=true;
  return true;
}
void endUsbMode() {
  usbActive=false;
  Serial.flush();
}
void serialTransferTask() {
  while(Serial.available()>0) {
    char ch=(char)Serial.read();
    if(ch=='\n') {
      processUsbCommand(usbCommandLine);
      usbCommandLine="";
      if(!usbActive) return;
    } else if(ch!='\r' && usbCommandLine.length()<256) {
      usbCommandLine+=ch;
    }
  }
}
void drawUsbScreen(bool ok) {
  header("USB TRANSFER");
  auto &d=M5Cardputer.Display;
  if(ok) {
    d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,52); d.print("USB SERIAL READY");
    d.setTextColor(WHITE,BG); d.setCursor(8,70); d.print("OPEN JOHNNY TRANSFER PAGE");
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,88); d.print("SEND / RECEIVE SD FILES");
  } else {
    d.setTextColor(UI_PINK,BG); d.setCursor(8,52); d.print("SD CARD NOT READY");
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,70); d.print("INSERT / CHECK MICROSD");
  }
  footer("ESC  RETURN TO JOHNNY");
}
String parentPath(const String &p) {
  if(p=="/") return "/";
  int n=p.lastIndexOf('/');
  return n<=0?"/":p.substring(0,n);
}
bool isPlayableAudio(String name) {
  name.toLowerCase();
  // Keep the player deliberately narrow: MP3 is the only advertised local format.
  return name.endsWith(".mp3");
}
void scanMusicRecursive(const String& folder,uint8_t depth) {
  // Bound directory traversal and allocations on large or slow SD cards.
  if(depth>4 || entryCount>=64) return;
  File dir=SD.open(folder,FILE_READ);
  if(!dir || !dir.isDirectory()){if(dir)dir.close();return;}
  File f=dir.openNextFile();
  while(f && entryCount<96) {
    String raw=String(f.name());
    bool isDir=f.isDirectory();
    String name=raw;int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);
    String full=folder=="/"?(String("/") + name):(folder + "/" + name);
    f.close();
    if(name.length() && name[0]!='.' && name!="System Volume Information" && name!="LOST.DIR") {
      if(isDir) scanMusicRecursive(full,depth+1);
      else if(isPlayableAudio(name)) {
        entries[entryCount]=name;entryPaths[entryCount]=full;entryIsDir[entryCount]=false;entryCount++;
      }
    }
    f=dir.openNextFile();
  }
  dir.close();
}
void scanDirectory(bool musicMode=false) {
  entryCount=0;selected=0;scroll=0;
  if(!sdReady)return;
  if(musicMode) {
    scanMusicRecursive("/",0);
    for(int i=0;i<entryCount-1;i++)for(int j=i+1;j<entryCount;j++)if(entries[j].compareTo(entries[i])<0){
      String t=entries[i];entries[i]=entries[j];entries[j]=t;
      t=entryPaths[i];entryPaths[i]=entryPaths[j];entryPaths[j]=t;
    }
    return;
  }
  File dir=SD.open(currentPath,FILE_READ);
  if(!dir || !dir.isDirectory()){if(dir)dir.close();return;}
  File f=dir.openNextFile();
  while(f && entryCount<96) {
    String name=String(f.name());int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);
    if(name.length()&&name!="System Volume Information"){
      entries[entryCount]=name;
      entryPaths[entryCount]=currentPath=="/"?(String("/") + name):(currentPath + "/" + name);
      entryIsDir[entryCount]=f.isDirectory();entryCount++;
    }
    f.close();f=dir.openNextFile();
  }
  dir.close();
  for(int i=0;i<entryCount-1;i++)for(int j=i+1;j<entryCount;j++)
    if((!entryIsDir[i]&&entryIsDir[j])||(entryIsDir[i]==entryIsDir[j]&&entries[j].compareTo(entries[i])<0)){
      String t=entries[i];entries[i]=entries[j];entries[j]=t;
      t=entryPaths[i];entryPaths[i]=entryPaths[j];entryPaths[j]=t;
      bool b=entryIsDir[i];entryIsDir[i]=entryIsDir[j];entryIsDir[j]=b;
    }
}void drawFiles(bool musicMode=false) {
  header(musicMode?"MUSIC LIBRARY":"SD EXPLORER");
  auto &d=M5Cardputer.Display;
  d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,42);
  String path=currentPath; if(path.length()>27)path="..."+path.substring(path.length()-24);
  d.print(path);
  if(!sdReady) {
    d.setTextColor(UI_PINK,BG); d.setCursor(8,68); d.print("NO SD CARD");
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,82); d.print("Check FAT32 card and contacts.");
    footer(); return;
  }
  int shown=0;
  if(currentPath!="/" && scroll==0) {
    d.setTextColor(UI_MUTED,BG); d.setCursor(10,57); d.print("../");
    shown++;
  }
  for(int i=scroll;i<entryCount && shown<5;i++) {
    int y=56+shown*12;
    bool on=i==selected;
    d.fillRect(8,y-1,224,11,on?PANEL:BG);
    d.setTextColor(on?WHITE:(entryIsDir[i]?UI_LAVENDER:UI_MUTED),on?PANEL:BG);
    d.setCursor(12,y);
    String n=entries[i]; if(n.length()>29)n=n.substring(0,26)+"...";
    d.printf("%c %s%s",on?'>':' ',n.c_str(),entryIsDir[i]?"/":"");
    shown++;
  }
  if(entryCount==0){d.setTextColor(UI_MUTED,BG);d.setCursor(10,68);d.print("EMPTY DIRECTORY");}
  footer(musicMode?"; / . SELECT  ENTER PLAY  ESC BACK":"; / . SELECT  ENTER OPEN  ESC BACK");
}

void drawMusicInfo(const String &name);
void drawClockScreen();void drawDiskManager();void drawProcessInfo();void drawThemeScreen();void drawChargingSaver();void serviceClockAlarm();
void drawRadioScreen();
void stopAudioLocked() {
  if(radioRecordStartRequested && radioRecordState==1) {
    radioRecordStartRequested=false;
    radioRecording=false;
    radioRecordStopRequested=true;
    radioRecordState=3;
  } else if(radioRecording) {
    if(audioOutput)audioOutput->flushRecording();
    radioRecording=false;
    radioRecordStopRequested=true;
    radioRecordState=3;
  }
  if(audioDecoder) {
    if(audioDecoder->isRunning()) audioDecoder->stop();
    delete audioDecoder;
    audioDecoder=nullptr;
  }
  if(audioInputBuffer) { delete audioInputBuffer; audioInputBuffer=nullptr; }
  if(audioRawSource) {
    audioRawSource->close();
    delete audioRawSource;
    audioRawSource=nullptr;
  }
  if(audioOutput) {
    audioOutput->stop();
    delete audioOutput;
    audioOutput=nullptr;
  }
  M5Cardputer.Speaker.stop();
  audioStarted=false;
  audioPaused=false;
  audioIsRadio=false;
  audioStopRequested=false;
}
void stopAudio() {
  audioStopRequested=true;
  if(audioMutex && xSemaphoreTake(audioMutex,pdMS_TO_TICKS(55))==pdTRUE){stopAudioLocked();audioTaskEnded=false;xSemaphoreGive(audioMutex);}
}
bool stopAudioAndWait(uint32_t timeoutMs=650){
  stopAudio();uint32_t started=millis();
  while((audioStarted||audioStopRequested)&&((uint32_t)(millis()-started)<timeoutMs)){
    if(audioMutex&&xSemaphoreTake(audioMutex,0)==pdTRUE){if(audioStopRequested)stopAudioLocked();xSemaphoreGive(audioMutex);}
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return !audioStarted&&!audioStopRequested;
}
AudioGenerator* createAudioDecoder(const String& extension) {
  if(extension=="mp3") return new AudioGeneratorMP3(mp3DecoderWorkspace,sizeof(mp3DecoderWorkspace));
  return nullptr;
}
bool startAudioSource(AudioFileSource* source, const String& title, const String& extension, bool radio) {
  if(!source) { audioStatusMessage="SOURCE ERROR"; return false; }
  if(!audioTaskReady || !audioMutex) {
    source->close(); delete source;
    audioStatusMessage="AUDIO TASK UNAVAILABLE";
    return false;
  }
  if(xSemaphoreTake(audioMutex,pdMS_TO_TICKS(20))!=pdTRUE) {
    source->close(); delete source;
    audioStatusMessage="AUDIO BUSY - TRY AGAIN";
    return false;
  }
  stopAudioLocked();
  audioRawSource=source;
  audioInputBuffer=new AudioFileSourceBuffer(audioRawSource,audioDecodeBuffer,sizeof(audioDecodeBuffer));
  if(!audioInputBuffer) {
    audioStatusMessage="NO BUFFER MEMORY";
    stopAudioLocked();
    if(audioMutex) xSemaphoreGive(audioMutex);
    return false;
  }
  audioOutput=new JohnnyAudioOutput(&M5Cardputer.Speaker);
  if(!audioOutput) {
    audioStatusMessage="NO AUDIO OUTPUT";
    stopAudioLocked();
    if(audioMutex) xSemaphoreGive(audioMutex);
    return false;
  }
  audioOutput->SetGain((float)audioVolume/21.0f);
  audioDecoder=createAudioDecoder(extension);
  if(!audioDecoder) {
    audioStatusMessage="UNSUPPORTED FORMAT";
    stopAudioLocked();
    if(audioMutex) xSemaphoreGive(audioMutex);
    return false;
  }
  if(!audioDecoder->begin(audioInputBuffer,audioOutput)) {
    Serial.printf("JOHNNY AUDIO: decoder begin failed; format=%s, bytes=%lu, heap=%lu, largest=%lu\\n",
      extension.c_str(),(unsigned long)(audioRawSource?audioRawSource->getSize():0),
      (unsigned long)ESP.getFreeHeap(),(unsigned long)ESP.getMaxAllocHeap());
    if(extension=="mp3") audioStatusMessage="MP3 INIT / MEMORY ERROR";
    else if(extension=="wav") audioStatusMessage="WAV MUST BE PCM 8/16 BIT";
    else if(extension=="ogg" || extension=="opus" || extension=="oga") audioStatusMessage="OGG MUST BE OPUS";
    else audioStatusMessage="AUDIO FORMAT / INIT ERROR";
    stopAudioLocked();
    if(audioMutex) xSemaphoreGive(audioMutex);
    return false;
  }
  audioNowTitle=title;
  if(radio){audioTrackPath="";audioTrackExtension="";audioTrackArtist="";audioTrackAlbum="";audioTrackTitle=title;audioCoverSize=0;}
  audioIsRadio=radio;
  audioPaused=false;
  audioTaskEnded=false;
  audioStarted=true;
  audioStatusMessage=radio?"LIVE RADIO":"PLAYING";
  M5Cardputer.Speaker.setVolume(255);
  if(audioMutex) xSemaphoreGive(audioMutex);
  return true;
}
String audioExtension(const String& path) {
  String name=path;
  name.toLowerCase();
  int dot=name.lastIndexOf('.');
  return dot>=0?name.substring(dot+1):"";
}
bool isOggOpusFile(const String& path) {
  File probe=SD.open(path,FILE_READ);
  if(!probe) return false;
  uint8_t bytes[160]={0};
  size_t count=probe.read(bytes,sizeof(bytes));
  probe.close();
  if(count<8 || memcmp(bytes,"OggS",4)!=0) return false;
  for(size_t i=0;i+8<=count;i++) {
    if(memcmp(bytes+i,"OpusHead",8)==0) return true;
  }
  return false;
}

uint32_t id3Synchsafe32(const uint8_t* b){return ((uint32_t)(b[0]&0x7f)<<21)|((uint32_t)(b[1]&0x7f)<<14)|((uint32_t)(b[2]&0x7f)<<7)|(uint32_t)(b[3]&0x7f);}
uint32_t id3BigEndian32(const uint8_t* b){return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|(uint32_t)b[3];}
String decodeId3Text(const uint8_t* data,size_t length){
  if(!data||length<2)return "";const uint8_t encoding=data[0];String value;value.reserve(min((size_t)96,length));
  if(encoding==1||encoding==2){size_t i=1;bool little=true;if(encoding==1&&length>=3&&data[1]==0xfe&&data[2]==0xff)little=false;if(encoding==1&&length>=3&&data[1]==0xff&&data[2]==0xfe)i=3;else if(encoding==1&&length>=3&&data[1]==0xfe&&data[2]==0xff)i=3;for(;i+1<length;i+=2){uint8_t lo=little?data[i]:data[i+1],hi=little?data[i+1]:data[i];if(lo==0&&hi==0)break;value+=(hi==0&&lo>=32)?(char)lo:'?';}}
  else{for(size_t i=1;i<length&&data[i]!=0;i++){uint8_t ch=data[i];value+=(ch>=32||ch=='\t')?(char)ch:' ';}}
  value.trim();return value;
}
bool extractApicImage(size_t length){
  if(length<8)return false;size_t p=0;const uint8_t encoding=audioCoverData[p++];while(p<length&&audioCoverData[p]!=0)p++;if(p>=length)return false;p++;if(p>=length)return false;p++;
  if(encoding==1||encoding==2){while(p+1<length&&!(audioCoverData[p]==0&&audioCoverData[p+1]==0))p+=2;if(p+1>=length)return false;p+=2;}
  else{while(p<length&&audioCoverData[p]!=0)p++;if(p>=length)return false;p++;}
  size_t start=length;for(size_t i=p;i+8<length;i++){if(audioCoverData[i]==0xff&&audioCoverData[i+1]==0xd8){start=i;audioCoverPng=false;break;}if(audioCoverData[i]==0x89&&audioCoverData[i+1]=='P'&&audioCoverData[i+2]=='N'&&audioCoverData[i+3]=='G'){start=i;audioCoverPng=true;break;}}
  if(start>=length)return false;size_t end=length;if(!audioCoverPng){for(size_t i=length-2;i>start;i--)if(audioCoverData[i]==0xff&&audioCoverData[i+1]==0xd9){end=i+2;break;}}
  if(end<=start||end-start>sizeof(audioCoverData))return false;memmove(audioCoverData,audioCoverData+start,end-start);audioCoverSize=(uint32_t)(end-start);return true;
}
bool loadSidecarCover(const String& trackPath){
  int slash=trackPath.lastIndexOf('/');String folder=slash>=0?trackPath.substring(0,slash+1):"/";String base=slash>=0?trackPath.substring(slash+1):trackPath;int dot=base.lastIndexOf('.');if(dot>0)base=base.substring(0,dot);
  String candidates[6]={folder+"cover.jpg",folder+"Cover.jpg",folder+"folder.jpg",folder+base+".jpg",folder+base+".png",String("/COVERS/")+base+".jpg"};
  for(const String& path:candidates){if(!SD.exists(path.c_str()))continue;File image=SD.open(path,FILE_READ);if(!image)continue;uint32_t size=(uint32_t)image.size();if(size>0&&size<=sizeof(audioCoverData)&&image.read(audioCoverData,size)==size){audioCoverSize=size;audioCoverPng=path.endsWith(".png")||path.endsWith(".PNG");image.close();return true;}image.close();}
  return false;
}

bool readFlacBE32(File& file,uint32_t& value){
  uint8_t b[4];if(file.read(b,4)!=4)return false;
  value=((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|(uint32_t)b[3];return true;
}
bool readFlacLE32(File& file,uint32_t& value){
  uint8_t b[4];if(file.read(b,4)!=4)return false;
  value=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);return true;
}
bool loadFlacMetadata(const String& path){
  File file=SD.open(path,FILE_READ);if(!file)return false;
  uint8_t marker[4];if(file.read(marker,4)!=4||memcmp(marker,"fLaC",4)!=0){file.close();return false;}
  bool lastBlock=false;uint8_t blocks=0;
  while(!lastBlock&&blocks++<64&&file.position()+4<=file.size()){
    uint8_t head[4];if(file.read(head,4)!=4)break;
    lastBlock=(head[0]&0x80)!=0;uint8_t type=head[0]&0x7f;
    uint32_t length=((uint32_t)head[1]<<16)|((uint32_t)head[2]<<8)|(uint32_t)head[3];
    uint32_t blockEnd=(uint32_t)file.position()+length;
    if(blockEnd>(uint32_t)file.size())break;
    if(type==4&&length>=8){
      uint32_t vendorLength=0,commentCount=0;
      if(!readFlacLE32(file,vendorLength)||vendorLength>blockEnd-file.position()){file.seek(blockEnd);continue;}
      file.seek((uint32_t)file.position()+vendorLength);
      if(!readFlacLE32(file,commentCount)){file.seek(blockEnd);continue;}
      commentCount=min(commentCount,(uint32_t)128);
      for(uint32_t i=0;i<commentCount&&file.position()+4<=blockEnd;i++){
        uint32_t commentLength=0;if(!readFlacLE32(file,commentLength)||commentLength>blockEnd-file.position())break;
        if(commentLength>0&&commentLength<=512){
          char comment[513];if(file.read((uint8_t*)comment,commentLength)!=commentLength)break;comment[commentLength]='\0';
          char* equal=strchr(comment,'=');if(equal){*equal='\0';String key=String(comment);key.toUpperCase();String value=String(equal+1);value.trim();
            if(key=="TITLE"&&value.length())audioTrackTitle=value;
            else if(key=="ARTIST"&&value.length())audioTrackArtist=value;
            else if(key=="ALBUM"&&value.length())audioTrackAlbum=value;
          }
        }else file.seek((uint32_t)file.position()+commentLength);
      }
    }else if(type==6&&length>=32){
      uint32_t pictureType=0,mimeLength=0,descriptionLength=0,width=0,height=0,depth=0,colors=0,imageLength=0;
      if(!readFlacBE32(file,pictureType)||!readFlacBE32(file,mimeLength)||mimeLength>blockEnd-file.position()){file.seek(blockEnd);continue;}
      char mime[24]={0};
      if(mimeLength<sizeof(mime)){file.read((uint8_t*)mime,mimeLength);mime[mimeLength]='\0';}
      else file.seek((uint32_t)file.position()+mimeLength);
      if(!readFlacBE32(file,descriptionLength)||descriptionLength>blockEnd-file.position()){file.seek(blockEnd);continue;}
      file.seek((uint32_t)file.position()+descriptionLength);
      if(!readFlacBE32(file,width)||!readFlacBE32(file,height)||!readFlacBE32(file,depth)||!readFlacBE32(file,colors)||!readFlacBE32(file,imageLength)){file.seek(blockEnd);continue;}
      if(imageLength>0&&imageLength<=sizeof(audioCoverData)&&imageLength<=blockEnd-file.position()&&file.read(audioCoverData,imageLength)==imageLength){
        bool jpg=imageLength>=4&&audioCoverData[0]==0xff&&audioCoverData[1]==0xd8;
        bool png=imageLength>=8&&audioCoverData[0]==0x89&&audioCoverData[1]=='P'&&audioCoverData[2]=='N'&&audioCoverData[3]=='G';
        if(jpg||png){audioCoverSize=imageLength;audioCoverPng=png;}
      }
    }
    file.seek(blockEnd);
  }
  file.close();return true;
}

void loadTrackMetadata(const String& path){
  // Keep track opening bounded: never scan artwork or walk an unbounded ID3 tag.
  audioTrackPath=path;
  audioTrackExtension=audioExtension(path);
  audioTrackArtist="";
  audioTrackAlbum="";
  audioTrackTitle=path.substring(path.lastIndexOf('/')+1);
  int dot=audioTrackTitle.lastIndexOf('.');
  if(dot>0)audioTrackTitle.remove(dot);
  audioCoverSize=0;
  audioCoverPng=false;
  if(audioTrackExtension!="mp3")return;

  File file=SD.open(path,FILE_READ);
  if(!file)return;
  uint8_t h[10]={0};
  if(file.read(h,sizeof(h))==sizeof(h) && memcmp(h,"ID3",3)==0 && h[3]>=3 && h[3]<=4){
    uint32_t tagSize=id3Synchsafe32(h+6);
    // Metadata is optional. Ignore oversized tags to avoid long blocking reads.
    if(tagSize<=8192U){
      uint32_t tagEnd=min((uint32_t)file.size(),10U+tagSize);
      uint8_t fh[10];
      for(uint8_t frames=0;frames<32 && (uint32_t)file.position()+10U<=tagEnd;frames++){
        if(file.read(fh,sizeof(fh))!=sizeof(fh) || fh[0]==0)break;
        char id[5]={(char)fh[0],(char)fh[1],(char)fh[2],(char)fh[3],0};
        uint32_t frameSize=h[3]==4?id3Synchsafe32(fh+4):id3BigEndian32(fh+4);
        uint32_t frameStart=(uint32_t)file.position();
        uint32_t frameEnd=frameStart+frameSize;
        // A zero-sized frame must terminate parsing; continuing here used to hang forever.
        if(frameSize==0 || frameEnd<frameStart || frameEnd>tagEnd || frameEnd>(uint32_t)file.size())break;
        if((strcmp(id,"TIT2")==0 || strcmp(id,"TPE1")==0 || strcmp(id,"TALB")==0) && frameSize<=256U){
          uint8_t txt[256];
          if(file.read(txt,frameSize)==frameSize){
            String value=decodeId3Text(txt,frameSize);
            if(strcmp(id,"TIT2")==0 && value.length())audioTrackTitle=value;
            else if(strcmp(id,"TPE1")==0)audioTrackArtist=value;
            else if(strcmp(id,"TALB")==0)audioTrackAlbum=value;
          }
        }
        if(!file.seek(frameEnd))break;
      }
    }
  }
  file.close();
}

bool startLocalAudio(const String& path) {
  if(!sdReady) { audioStatusMessage="NO SD CARD"; return false; }
  if(!stopAudioAndWait(650)){audioStatusMessage="AUDIO BUSY TIMEOUT";return false;}
  loadTrackMetadata(path);
  const String ext=audioExtension(path);
  if(ext!="mp3") {audioStatusMessage="MP3 ONLY IN THIS BUILD";return false;}
  AudioFileSourceSD* source=new AudioFileSourceSD();
  if(!source || !source->open(path.c_str())) {
    if(source) delete source;
    audioStatusMessage="CANNOT OPEN FILE";
    return false;
  }
  String title=path.substring(path.lastIndexOf('/')+1);
  if(!startAudioSource(source,title,ext,false)) return false;
  return true;
}


bool startRadioStream(const String& url, const String& title) {
  if(!stopAudioAndWait(650)){audioStatusMessage="AUDIO BUSY TIMEOUT";radioMessage=audioStatusMessage;return false;}
  if(WiFi.status()!=WL_CONNECTED) {
    audioStatusMessage="CONNECT WI-FI FIRST";
    radioMessage=audioStatusMessage;
    return false;
  }
  if(!url.startsWith("http://")) {
    audioStatusMessage="USE HTTP STREAM URL";
    radioMessage=audioStatusMessage;
    return false;
  }
  AudioFileSourceICYStream* stream=new AudioFileSourceICYStream();
  if(!stream || !stream->open(url.c_str())) {
    if(stream) delete stream;
    audioStatusMessage="RADIO CONNECTION FAILED";
    radioMessage=audioStatusMessage;
    return false;
  }
  const bool ok=startAudioSource(stream,title,"mp3",true);
  radioMessage=ok?"LIVE / CONNECTED":audioStatusMessage;
  return ok;
}
uint8_t audioProgressPercent(){if(!audioMutex||xSemaphoreTake(audioMutex,0)!=pdTRUE)return 0;uint32_t pos=0,size=0;if(audioRawSource){size=audioRawSource->getSize();pos=audioRawSource->getPos();if(audioInputBuffer){uint32_t n=audioInputBuffer->getFillLevel();pos=pos>n?pos-n:0;}}xSemaphoreGive(audioMutex);if(!size)return 0;if(pos>size)pos=size;return (uint8_t)((uint64_t)pos*100ULL/size);}
bool seekAudioStep(int direction){
  if(!audioStarted||audioIsRadio||!audioMutex)return false;
  if(xSemaphoreTake(audioMutex,pdMS_TO_TICKS(15))!=pdTRUE){audioStatusMessage="AUDIO BUSY";return false;}
  if(!audioRawSource||!audioInputBuffer||!audioDecoder){xSemaphoreGive(audioMutex);return false;}
  uint32_t size=audioRawSource->getSize(),pos=audioRawSource->getPos(),buffered=audioInputBuffer->getFillLevel();pos=pos>buffered?pos-buffered:0;
  if(size<4096){xSemaphoreGive(audioMutex);return false;}
  int64_t target=(int64_t)pos+(int64_t)direction*(int64_t)max((uint32_t)4096,size/20U);if(target<0)target=0;if(target>(int64_t)size-1)target=(int64_t)size-1;
  if(audioTrackExtension!="mp3"&&audioTrackExtension!="aac"){audioStatusMessage="SEEK NOT SUPPORTED";xSemaphoreGive(audioMutex);return false;}
  bool paused=audioPaused;audioDecoder->stop();delete audioDecoder;audioDecoder=nullptr;delete audioInputBuffer;audioInputBuffer=nullptr;
  if(!audioRawSource->seek((int32_t)target,SEEK_SET))audioRawSource->seek(0,SEEK_SET);
  audioInputBuffer=new AudioFileSourceBuffer(audioRawSource,audioDecodeBuffer,sizeof(audioDecodeBuffer));audioDecoder=createAudioDecoder(audioTrackExtension);
  bool ok=audioInputBuffer&&audioDecoder&&audioDecoder->begin(audioInputBuffer,audioOutput);
  if(ok){audioStarted=true;audioPaused=paused;audioTaskEnded=false;audioStatusMessage=paused?"PAUSED":"PLAYING";audioEqualizer.reset();}
  else{if(audioDecoder){delete audioDecoder;audioDecoder=nullptr;}if(audioInputBuffer){delete audioInputBuffer;audioInputBuffer=nullptr;}audioStarted=false;audioPaused=true;audioStatusMessage="SEEK FAILED";}
  xSemaphoreGive(audioMutex);return ok;
}
void serviceMusicProgress(){
  if(screen!=MUSIC||!musicNowPlaying||!audioStarted||screenSaverActive)return;
  if((uint32_t)(millis()-musicProgressLastDraw)<500UL)return;
  musicProgressLastDraw=millis();uint8_t pct=audioProgressPercent();if(pct==musicProgressLastValue)return;musicProgressLastValue=pct;
  auto& d=M5Cardputer.Display;d.drawFastHLine(78,96,151,BG);d.drawFastHLine(78,96,(int)pct*151/100,UI_PINK);d.fillCircle(78+(int)pct*151/100,96,3,UI_LAVENDER);d.fillRect(78,101,34,12,BG);d.setTextColor(UI_MUTED,BG);d.setCursor(78,103);d.printf("%3u%%",pct);
}
void serviceAudio() {
  bool ended=false;
  bool wasRadio=false;
  if(audioMutex && xSemaphoreTake(audioMutex,0)==pdTRUE) {
    if(audioTaskEnded) {
      ended=true;
      wasRadio=audioTaskEndedWasRadio;
      audioTaskEnded=false;
    }
    xSemaphoreGive(audioMutex);
  }
  if(!ended) return;
  stopAudio();audioStatusMessage=wasRadio?"STREAM ENDED":"TRACK FINISHED";if(wasRadio)radioMessage=audioStatusMessage;
  else if(screen==MUSIC&&musicNowPlaying&&entryCount>0){
    if(audioRepeatMode==2)playSelectedTrack();
    else if(audioShuffle)moveMusicTrack(1);
    else if(selected<entryCount-1)moveMusicTrack(1);
    else if(audioRepeatMode==1){selected=0;playSelectedTrack();}
    else {musicNowPlaying=false;audioStatusMessage="PLAYBACK COMPLETE";drawFiles(true);}
  }
  if(!screenSaverActive){if(screen==MUSIC&&musicNowPlaying)drawMusicInfo(audioTrackTitle.length()?audioTrackTitle:audioNowTitle);else if(screen==RADIO)drawRadioScreen();}
}
void audioTask(void* parameter) {
  (void)parameter;audioTaskReady=true;
  for(;;){
    if(audioMutex&&xSemaphoreTake(audioMutex,0)==pdTRUE){
      if(audioStopRequested){stopAudioLocked();audioTaskEnded=false;}
      else if(audioStarted&&!audioPaused&&audioDecoder){if(!audioDecoder->loop()){audioTaskEndedWasRadio=audioIsRadio;audioTaskEnded=true;audioStarted=false;audioPaused=true;}}
      else if(synthPadRequest>=0&&!audioStarted){
        int pad=synthPadRequest;synthPadRequest=-1;char path[40];snprintf(path,sizeof(path),"/JOHNNY/PADS/PAD%d.WAV",pad+1);
        File sample=SD.open(path,FILE_READ);
        if(sample&&sample.size()>44){uint8_t head[44];bool valid=sample.read(head,44)==44&&memcmp(head,"RIFF",4)==0&&memcmp(head+8,"WAVE",4)==0;
          if(valid){size_t count=min((size_t)8192,(size_t)(sample.size()-44)/2);size_t got=sample.read((uint8_t*)synthPadSamples,count*2);count=got/2;if(count)M5Cardputer.Speaker.playRaw(synthPadSamples,count,16000,false,1,SYNTH_AUDIO_CHANNEL,false);}
          sample.close();
        }
      }
      xSemaphoreGive(audioMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
void toggleAudioPause() {
  if(!audioMutex || xSemaphoreTake(audioMutex,pdMS_TO_TICKS(5))!=pdTRUE) {
    audioStatusMessage="AUDIO BUSY";
    return;
  }
  audioPaused=!audioPaused;
  xSemaphoreGive(audioMutex);
}
void setAudioVolume(int value) {
  if(!audioMutex || xSemaphoreTake(audioMutex,pdMS_TO_TICKS(5))!=pdTRUE) {
    audioStatusMessage="AUDIO BUSY";
    return;
  }
  audioVolume=constrain(value,0,21);
  if(audioOutput) audioOutput->SetGain((float)audioVolume/21.0f);
  xSemaphoreGive(audioMutex);
}
void drawMusicInfo(const String &name){
  header("JOHNNY MUSIC");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_MUTED,BG);d.setCursor(8,39);d.print("NOW PLAYING // LOCAL AUDIO");d.fillRoundRect(7,49,77,66,5,PANEL);
  bool cover=false;if(audioCoverSize>0)cover=audioCoverPng?d.drawPng(audioCoverData,audioCoverSize,9,51,73,62):d.drawJpg(audioCoverData,audioCoverSize,9,51,73,62);
  if(!cover){d.fillCircle(45,82,28,0x2412);d.drawCircle(45,82,25,UI_PURPLE);d.drawCircle(45,82,17,UI_PINK);d.drawCircle(45,82,6,UI_LAVENDER);}
  String title=audioTrackTitle.length()?audioTrackTitle:name;if(title.length()>22)title=title.substring(0,19)+"...";
  String artist=audioTrackArtist.length()?audioTrackArtist:"UNKNOWN ARTIST";if(artist.length()>21)artist=artist.substring(0,18)+"...";
  String album=audioTrackAlbum.length()?audioTrackAlbum:"LOCAL LIBRARY";if(album.length()>21)album=album.substring(0,18)+"...";
  d.setTextColor(WHITE,BG);d.setCursor(91,53);d.print(title);d.setTextColor(UI_LAVENDER,BG);d.setCursor(91,68);d.print(artist);d.setTextColor(UI_MUTED,BG);d.setCursor(91,82);d.print(album);
  uint8_t pct=audioProgressPercent();d.drawRoundRect(91,94,140,6,3,UI_EDGE);d.fillRoundRect(92,95,max(0,(int)pct*138/100),4,2,UI_PINK);
  d.setTextColor(UI_CYAN,BG);d.setCursor(91,105);d.printf("%3u%%  VOL %02d",pct,audioVolume);
  d.setTextColor(audioStarted?(audioPaused?UI_PINK:UI_LAVENDER):UI_MUTED,BG);d.setCursor(91,117);d.print(audioStarted?(audioPaused?"PAUSED":"PLAYING"):audioStatusMessage.substring(0,19));
  footer("ENTER PAUSE  P STOP  , / TRACK  ; . SEEK  - / + VOL  A SET ALARM");
}
void drawRadioScreen() {
  header("WEB RADIO");
  auto &d=M5Cardputer.Display;
  if(radioEditingUrl) {
    d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,43); d.print("CUSTOM HTTP STREAM URL");
    d.setTextColor(WHITE,BG); d.setCursor(8,61);
    String shown=radioEntry; if(shown.length()>31) shown=shown.substring(shown.length()-31);
    d.print(shown);
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,83); d.print("TYPE URL, THEN ENTER");
    d.setCursor(8,97); d.print("HTTP STREAMS (NOT HTTPS)");
    footer("ENTER CONNECT  ESC CANCEL");
    return;
  }
  d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,42);
  d.print(WiFi.status()==WL_CONNECTED?"WI-FI ONLINE":"WI-FI OFFLINE");
  for(int i=0;i<RADIO_STATION_COUNT+1;i++) {
    int y=54+i*10;
    bool active=i==radioSelected;
    uint16_t fill=active?PANEL:BG;
    d.fillRoundRect(7,y-1,225,10,2,fill);
    d.setTextColor(active?WHITE:UI_MUTED,fill); d.setCursor(12,y);
    const char* label=(i<RADIO_STATION_COUNT)?radioStationNames[i]:"CUSTOM STREAM URL";
    d.print(active?"> ":"  "); d.print(label);
    if(audioIsRadio && audioStarted && i==radioSelected) {
      d.setTextColor(UI_PINK,fill); d.setCursor(205,y); d.print("ON");
    }
  }
  d.setTextColor(audioStarted && audioIsRadio?UI_PINK:UI_MUTED,BG); d.setCursor(8,108);
  String status=audioIsRadio?(audioPaused?"RADIO PAUSED":"LIVE STREAM"):radioMessage;
  if(status=="SELECT A STATION"||status.length()==0)status=audioStatusMessage;
  if(radioRecordState==1)status="RECORDER STARTING";
  else if(radioRecordState==2)status="REC  "+radioRecordPath.substring(max(0,(int)radioRecordPath.length()-24));
  else if(radioRecordState==3)status="SAVING RECORDING...";
  else if(radioRecordState==4)status="SAVED: "+radioRecordPath.substring(max(0,(int)radioRecordPath.length()-24));
  else if(radioRecordState==5)status="RECORDING ERROR";
  d.print(status.substring(0,30));
  footer("R REC/STOP  SPACE PAUSE");
}
void playSelectedTrack() {
  if(selected<0 || selected>=entryCount || entryIsDir[selected]) return;
  String path=entryPaths[selected];
  if(path.length()==0)path=currentPath=="/"?(String("/") + entries[selected]):(currentPath + "/" + entries[selected]);
  audioStatusMessage="LOADING";
  if(!startLocalAudio(path)){
    musicNowPlaying=false;drawFiles(true);
    M5Cardputer.Display.setTextColor(UI_PINK,BG);M5Cardputer.Display.setCursor(8,108);
    M5Cardputer.Display.print(audioStatusMessage.substring(0,28));return;
  }
  musicNowPlaying=true;musicProgressLastValue=255;drawMusicInfo(entries[selected]);
}
void moveMusicTrack(int direction) {
  if(entryCount<=0)return;
  if(audioShuffle&&direction>0){int choices=0;for(int i=0;i<entryCount;i++)if(!entryIsDir[i]&&isPlayableAudio(entries[i]))choices++;if(choices){int pick=random(choices);for(int i=0;i<entryCount;i++)if(!entryIsDir[i]&&isPlayableAudio(entries[i]))if(pick--==0){selected=i;playSelectedTrack();return;}}}
  int candidate=selected;
  for(int tries=0;tries<entryCount;tries++) {
    candidate=(candidate+direction+entryCount)%entryCount;
    if(!entryIsDir[candidate] && isPlayableAudio(entries[candidate])) {
      selected=candidate;
      playSelectedTrack();
      return;
    }
  }
}
void openFile(bool musicMode) {
  if(!sdReady || selected<0 || selected>=entryCount) return;
  String full=entryPaths[selected];
  if(full.length()==0){full=currentPath;if(full!="/")full+="/";full+=entries[selected];}
  if(entryIsDir[selected]) {
    musicNowPlaying=false; currentPath=full; scanDirectory(musicMode); drawFiles(musicMode); return;
  }
  selectedFile=full;
  if(musicMode) playSelectedTrack();
  else {
    File f=SD.open(full); header("FILE DETAILS");
    auto &d=M5Cardputer.Display;
    d.setTextColor(WHITE,BG); d.setCursor(8,53); d.print(entries[selected].substring(0,30));
    if(f) {
      d.setTextColor(UI_MUTED,BG); d.setCursor(8,72); d.printf("%lu bytes",(unsigned long)f.size());
      d.setCursor(8,88); d.print("TYPE: FILE"); f.close();
    } else {d.setTextColor(UI_PINK,BG);d.setCursor(8,72);d.print("UNABLE TO OPEN FILE");}
    footer();
  }
}
void drawEqScreen();
void drawMicRecorder();
void stopMicRecording();
void serviceMicRecorder();
void seedLife();
void drawLife();
bool loadLife();
void openLegacyApp();
void openApp(){
  if(screen==HOME){screen=selected==0?HACKMENU:(selected==1?GAMESMENU:(selected==2?MUSICMENU:SYSTEMMENU));selected=0;drawCategoryMenu();return;}
  int item=selected;Screen menu=screen;
  if(menu==HACKMENU){appReturnMenu=HACKMENU;appReturnSelected=item;if(item==0){screen=NETWORK;startWifiSurvey();return;}if(item==1){screen=BLESCAN;startBleScan();drawBleScreen();return;}if((item>=2&&item<=4)||item==7){hackInputMode=(uint8_t)(item==7?3:item-2);hackInputValue="";hackResultMessage="";hackInputDone=false;screen=HACKINPUT;drawHackInput();return;}if(item==6){screen=HACKFX;hackVisualFx=true;hackFxLastFrame=0;drawHackFxScreen();return;}screen=HACKINFO;drawHackInfo();return;}
  if(menu!=MUSICMENU&&menu!=SYSTEMMENU&&menu!=GAMESMENU)return;
  appReturnMenu=menu;appReturnSelected=item;
  if(menu==SYSTEMMENU&&item==5){screen=BLESCAN;startBleScan();drawBleScreen();return;}
  if(menu==SYSTEMMENU&&item==6){screen=CLOCKSCREEN;lastClockDraw=0;drawClockScreen();return;}
  if(menu==SYSTEMMENU&&item==7){screen=DISKMANAGER;drawDiskManager();return;}
  if(menu==SYSTEMMENU&&item==8){screen=PROCESSINFO;drawProcessInfo();return;}
  if(menu==SYSTEMMENU&&item==9){screen=THEMES;themeSelection=themeMode;drawThemeScreen();return;}
  screen=HOME;
  if(menu==MUSICMENU){if(item==5){screen=MICREC;micRecording=false;micRecordStatus="READY";drawMicRecorder();return;}const int map[]={0,1,2,3,7};selected=map[item];}
  else if(menu==SYSTEMMENU){if(item==3){screen=UPDATEINFO;updateMessage="PRESS ENTER TO INSTALL LATEST";drawUpdateInfo();return;}if(item==4){selected=9;openLegacyApp();return;}const int map[]={4,6,5};selected=map[item];}
  else selected=8;
  openLegacyApp();
}
void openLegacyApp() {
  switch(selected) {
    case 0: if(!stopAudioAndWait(650)){screen=MUSIC;drawMusicInfo(audioNowTitle);break;}stopMicCapture();audioEqualizer.reset();screen=PIANO;pianoScreen();break;
    case 1:
      if(!stopAudioAndWait(650)){screen=MUSIC;drawMusicInfo(audioNowTitle);break;}stopMicCapture();screen=TUNER;tunerInstrument=0;
      tunerFrequency=0; tunerFilteredFrequency=0; tunerHasSignal=false;
      tunerLastSignalAt=0; tunerLastSample=0; tunerScreen(); break;
    case 2:
      if(!stopAudioAndWait(650)){screen=MUSIC;drawMusicInfo(audioNowTitle);break;}
      screen=MUSIC;musicNowPlaying=false;currentPath="/";scanDirectory(true);drawFiles(true);break;
    case 3:
      if(!audioIsRadio && audioStarted && !stopAudioAndWait(650)){screen=MUSIC;drawMusicInfo(audioNowTitle);break;}
      screen=RADIO; radioEditingUrl=false; radioSelected=0; drawRadioScreen(); break;
    case 4:
      if(!stopAudioAndWait(650)){screen=MUSIC;drawMusicInfo(audioNowTitle);break;}
      {uint32_t started=millis();while(radioRecordState==3&&millis()-started<1500UL)vTaskDelay(pdMS_TO_TICKS(5));}
      screen=FILES;currentPath="/";scanDirectory(false);drawFiles();break;
    case 5: screen=NETWORK; wifiConfigStage=0; lastWiFiStatus=-1; drawNetworkScreen(); break;
    case 6: screen=USBMODE;drawUsbScreen(beginUsbMode());break;
    case 7: eqReturnScreen=MUSICMENU;screen=EQSCREEN;selected=4;drawEqScreen();break;
    case 8: screen=LIFE;if(!loadLife())seedLife();drawLife();break;
    case 9: screen=ABOUT;drawDeviceStatus();break;
    default:
      screen=ABOUT; header("SYSTEM");
      M5Cardputer.Display.setTextColor(WHITE,BG); M5Cardputer.Display.setCursor(8,53); M5Cardputer.Display.print("JOHNNY / CARDPUTER-ADV");
      M5Cardputer.Display.setCursor(8,69); M5Cardputer.Display.print("ESP32-S3  |  8 MB FLASH");
      M5Cardputer.Display.setCursor(8,85); M5Cardputer.Display.printf("SD: %s",sdReady?"MOUNTED":"NOT FOUND");
      M5Cardputer.Display.setCursor(8,91); M5Cardputer.Display.printf("WI-FI: %s",WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE");
      {
        int battery=batteryPercent(); int16_t mv=batteryMillivolts(); bool charging=batteryCharging();
        M5Cardputer.Display.setCursor(8,103);
        if(battery>=0 && mv>0) M5Cardputer.Display.printf("BATTERY: %d%%  %.2fV",constrain(battery,0,100),mv/1000.0f);
        else if(battery>=0) M5Cardputer.Display.printf("BATTERY: %d%%  VOLT N/A",constrain(battery,0,100));
        else M5Cardputer.Display.print("BATTERY: SENSOR UNAVAILABLE");
        M5Cardputer.Display.setCursor(8,115); M5Cardputer.Display.setTextColor(charging?UI_PINK:UI_MUTED,BG);
        M5Cardputer.Display.print(charging?"CHARGING [USB POWER]":(battery>=0 && battery<=15?"LOW BATTERY":"ON BATTERY"));
      }
      footer("ESC BACK"); break;
  }
}
void setEqBandGain(int band,float gain){
  if(band<0||band>=(int)JOHNNY_EQ_BANDS)return;
  if(audioMutex)xSemaphoreTake(audioMutex,portMAX_DELAY);
  audioEqGainDb[band]=constrain(gain,-6.0f,6.0f);audioEqualizer.setBand((size_t)band,audioEqGainDb[band]);
  if(audioMutex)xSemaphoreGive(audioMutex);
  preferences.putInt("eqpreset",-1);preferences.putFloat((String("eq")+String(band)).c_str(),audioEqGainDb[band]);eqPreset=-1;
}
void applyEqPreset(int preset){
  preset=(preset+4)%4;
  if(audioMutex)xSemaphoreTake(audioMutex,portMAX_DELAY);
  eqPreset=preset;
  for(size_t i=0;i<JOHNNY_EQ_BANDS;i++)audioEqGainDb[i]=eqPresets[preset][i];
  audioEqualizer.setAll(audioEqGainDb);
  if(audioMutex)xSemaphoreGive(audioMutex);
  preferences.putInt("eqpreset",preset);
  for(size_t i=0;i<JOHNNY_EQ_BANDS;i++)preferences.putFloat((String("eq")+String((int)i)).c_str(),audioEqGainDb[i]);
}
void drawEqScreen(){
  header("AUDIO EQUALIZER");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_PINK,BG);d.setCursor(8,43);d.printf("PROFILE: %s",eqPreset>=0?eqPresetNames[eqPreset]:"CUSTOM");
  const char* labels[]={"80 Hz","250 Hz","1 kHz","4 kHz","10 kHz"};
  for(int i=0;i<(int)JOHNNY_EQ_BANDS;i++){
    int y=57+i*11;bool active=i==eqSelectedBand;
    d.setTextColor(active?WHITE:UI_MUTED,BG);d.setCursor(8,y);d.print(labels[i]);
    d.drawFastHLine(85,y+4,130,0x5140);d.drawFastVLine(150,y+1,7,UI_LAVENDER);
    int marker=150+(int)lroundf(audioEqGainDb[i]*8.0f);
    d.fillCircle(constrain(marker,102,198),y+4,3,active?UI_PINK:UI_PURPLE);
    d.setTextColor(active?UI_PINK:UI_LAVENDER,BG);d.setCursor(205,y);d.printf("%+.1f",audioEqGainDb[i]);
  }
  footer("UP/DN BAND  LEFT/RIGHT GAIN  P PRESET");
}
void clearLife(){memset(lifeGrid,0,sizeof(lifeGrid));lifeGeneration=0;lifeRunning=false;}
void saveLife(){
  if(!sdReady)return;
  SD.mkdir("/JOHNNY");SD.remove("/JOHNNY/life.sav");
  File file=SD.open("/JOHNNY/life.sav",FILE_WRITE);if(!file)return;
  const uint8_t magic[4]={'J','L','I','F'};
  file.write(magic,sizeof(magic));file.write((const uint8_t*)&lifeGeneration,sizeof(lifeGeneration));
  file.write((const uint8_t*)&lifeIntervalMs,sizeof(lifeIntervalMs));
  file.write((const uint8_t*)lifeGrid,sizeof(lifeGrid));file.flush();file.close();
}
bool loadLife(){  if(!sdReady)return false;File file=SD.open("/JOHNNY/life.sav",FILE_READ);if(!file)return false;
  uint8_t magic[4]{};uint32_t generation=0;uint16_t interval=180;
  bool ok=file.read(magic,4)==4&&memcmp(magic,"JLIF",4)==0;
  ok=ok&&file.read((uint8_t*)&generation,sizeof(generation))==sizeof(generation);
  ok=ok&&file.read((uint8_t*)&interval,sizeof(interval))==sizeof(interval);
  ok=ok&&file.read((uint8_t*)lifeGrid,sizeof(lifeGrid))==sizeof(lifeGrid);file.close();  if(!ok)return false;lifeGeneration=generation;lifeIntervalMs=constrain((int)interval,50,1000);lifeRunning=false;return true;
}

void seedLife(){
  for(int y=0;y<LIFE_ROWS;y++)for(int x=0;x<LIFE_COLS;x++)lifeGrid[y][x]=(random(100)<27);
  lifeGeneration=0;lifeRunning=true;lifeLastStep=millis();
}
void toggleLifeCell(){lifeGrid[lifeCursorY][lifeCursorX]=!lifeGrid[lifeCursorY][lifeCursorX];}
void stepLife(){
  for(int y=0;y<LIFE_ROWS;y++)for(int x=0;x<LIFE_COLS;x++){
    int neighbours=0;
    for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){
      if(dx==0&&dy==0)continue;
      int nx=x+dx,ny=y+dy;
      if(nx>=0&&nx<LIFE_COLS&&ny>=0&&ny<LIFE_ROWS&&lifeGrid[ny][nx])neighbours++;
    }
    lifeNext[y][x]=lifeGrid[y][x]?(neighbours==2||neighbours==3):(neighbours==3);  }
  memcpy(lifeGrid,lifeNext,sizeof(lifeGrid));lifeGeneration++;lifeLastStep=millis();
}
void drawLife(){
  header("CONWAY LIFE");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,42);
  d.printf("GEN %lu  %s  %ums",(unsigned long)lifeGeneration,lifeRunning?"RUN":"PAUSE",lifeIntervalMs);
  const int ox=24,oy=48,cell=4;
  for(int y=0;y<LIFE_ROWS;y++)for(int x=0;x<LIFE_COLS;x++){
    int px=ox+x*cell,py=oy+y*cell;
    if(lifeGrid[y][x])d.fillRect(px+1,py+1,3,3,UI_PINK);else d.fillRect(px+1,py+1,3,3,BG);
    if(x==lifeCursorX&&y==lifeCursorY)d.drawRect(px,py,cell,cell,UI_LAVENDER);
  }
  footer("SPACE RUN/PAUSE  N STEP  ARROWS MOVE");
}
void startRadioRecording(){
  if(!sdReady||!audioStarted||!audioIsRadio||WiFi.status()!=WL_CONNECTED){radioRecordState=5;return;}  if(!radioRecordQueue||!radioRecorderTaskReady||radioRecordState==1||radioRecordState==2||radioRecordState==3)return;
  SD.mkdir("/RECORDINGS");String candidate;
  for(int i=1;i<=9999;i++){char name[32];snprintf(name,sizeof(name),"/RECORDINGS/RADIO%04d.WAV",i);candidate=String(name);if(!SD.exists(candidate))break;}
  if(candidate.length()==0||SD.exists(candidate)){radioRecordState=5;return;}
  radioRecordPath=candidate;radioRecordFailed=false;radioRecordSampleRate=0;radioRecordDataBytes=0;
  radioRecordState=1;radioRecordStartRequested=true;
}
void stopRadioRecording(){
  if(radioRecordState!=1&&radioRecordState!=2)return;
  if(audioMutex)xSemaphoreTake(audioMutex,portMAX_DELAY);
  if(audioOutput)audioOutput->flushRecording();
  radioRecording=false;radioRecordStopRequested=true;radioRecordState=3;
  if(audioMutex)xSemaphoreGive(audioMutex);
}
void writeWavHeader(File& file,uint32_t dataBytes,uint32_t sampleRate){
  uint8_t h[44]{};memcpy(h,"RIFF",4);uint32_t riff=36U+dataBytes;memcpy(h+4,&riff,4);
  memcpy(h+8,"WAVEfmt ",8);uint32_t fmt=16;memcpy(h+16,&fmt,4);
  uint16_t format=1,channels=1,bits=16,align=2;
  memcpy(h+20,&format,2);memcpy(h+22,&channels,2);memcpy(h+24,&sampleRate,4);
  uint32_t byteRate=sampleRate*2U;memcpy(h+28,&byteRate,4);memcpy(h+32,&align,2);memcpy(h+34,&bits,2);
  memcpy(h+36,"data",4);memcpy(h+40,&dataBytes,4);file.seek(0);file.write(h,sizeof(h));
}
void drawMicRecorder(){
  header("MIC RECORDER");auto& d=M5Cardputer.Display;
  d.setTextColor(micRecording?UI_PINK:UI_LAVENDER,BG);d.setCursor(8,49);
  d.print(micRecording?"RECORDING":"MICROPHONE READY");
  d.setTextColor(UI_MUTED,BG);d.setCursor(8,68);d.print(micRecordStatus.substring(0,30));
  d.setCursor(8,84);d.printf("%.1f SEC",(float)micRecordBytes/32000.0f);
  d.setCursor(8,100);d.print(micRecordPath.substring(max(0,(int)micRecordPath.length()-28)));
  footer("ENTER / SPACE START-STOP");
}
void startMicRecording(){
  if(!sdReady){micRecordStatus="NO SD CARD";drawMicRecorder();return;}
  String candidate;
  if(screen==PIANO){SD.mkdir("/JOHNNY");SD.mkdir("/JOHNNY/PADS");char path[40];snprintf(path,sizeof(path),"/JOHNNY/PADS/PAD%u.WAV",synthPadSelected+1);candidate=String(path);if(SD.exists(candidate))SD.remove(candidate);micRecordPadIndex=synthPadSelected;}
  else{SD.mkdir("/RECORDINGS");for(int i=1;i<=9999;i++){char name[32];snprintf(name,sizeof(name),"/RECORDINGS/MIC%04d.WAV",i);candidate=String(name);if(!SD.exists(candidate))break;}}
  if(candidate.length()==0||SD.exists(candidate)){micRecordStatus="NO FREE FILENAME";micRecordPadIndex=-1;drawMicRecorder();return;}
  micRecordFile=SD.open(candidate,FILE_WRITE);if(!micRecordFile){micRecordStatus="FILE OPEN FAILED";if(screen==PIANO)pianoScreen();else drawMicRecorder();return;}
  uint8_t blank[44]{};micRecordFile.write(blank,sizeof(blank));micRecordPath=candidate;micRecordBytes=0;micRecording=true;micRecordStatus="RECORDING PCM 16 KHZ";micRecordLastDraw=0;if(screen==PIANO)pianoScreen();else drawMicRecorder();
}
void stopMicRecording(){
  if(!micRecording)return;
  micRecording=false;
  if(micRecordFile){writeWavHeader(micRecordFile,micRecordBytes,16000);micRecordFile.flush();micRecordFile.close();}
  stopMicCapture();micRecordStatus=micRecordPadIndex>=0?"PAD SAMPLE SAVED":"SAVED TO MICROSD";micRecordPadIndex=-1;if(screen==PIANO)pianoScreen();else drawMicRecorder();
}
void serviceMicRecorder(){
  if(!micRecording)return;
  if(!captureMic(micRecordSamples,3072,420)){if(millis()-micRecordLastDraw>500){micRecordLastDraw=millis();if(!screenSaverActive){if(screen==PIANO)pianoScreen();else drawMicRecorder();}}return;}
  size_t bytes=sizeof(micRecordSamples);
  if(!micRecordFile||micRecordFile.write((const uint8_t*)micRecordSamples,bytes)!=bytes){micRecordStatus="SD WRITE ERROR";stopMicRecording();return;}
  micRecordBytes+=(uint32_t)bytes;
  if(millis()-micRecordLastDraw>500){micRecordLastDraw=millis();if(!screenSaverActive){if(screen==PIANO)pianoScreen();else drawMicRecorder();}}
}
void radioRecorderTask(void* parameter){
  (void)parameter;File output;radioRecorderTaskReady=true;
  for(;;){
    if(radioRecordStartRequested){
      radioRecordStartRequested=false;if(output)output.close();
      if(!SD.exists("/RECORDINGS"))SD.mkdir("/RECORDINGS");
      output=SD.open(radioRecordPath,FILE_WRITE);
      if(output){uint8_t blank[44]{};output.write(blank,sizeof(blank));radioRecordDataBytes=0;radioRecording=true;radioRecordState=2;}
      else{radioRecording=false;radioRecordState=5;}
    }
    RadioRecordBlock block{};
    if(radioRecordQueue&&xQueueReceive(radioRecordQueue,&block,pdMS_TO_TICKS(40))==pdTRUE){
      if(output&&block.count>0){
        size_t bytes=(size_t)block.count*sizeof(int16_t),written=output.write((const uint8_t*)block.samples,bytes);
        if(written==bytes)radioRecordDataBytes+=(uint32_t)written;
        else{radioRecording=false;radioRecordFailed=true;radioRecordStopRequested=true;radioRecordState=3;}
      }
    }
    if(radioRecordStopRequested&&radioRecordQueue&&uxQueueMessagesWaiting(radioRecordQueue)==0){
      radioRecordStopRequested=false;
      if(output){uint32_t rate=radioRecordSampleRate>0?radioRecordSampleRate:22050U;writeWavHeader(output,radioRecordDataBytes,rate);output.flush();output.close();radioRecordState=radioRecordFailed?5:4;}
      else radioRecordState=5;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

const char* themeNames[]={"JOHNNY PURPLE","BRUCE DARK","CYBERPUNK","MATRIX","AMBER","MINIMAL"};
const uint16_t themePalette[6][10]={
 {0x1008,0x280C,0xA01F,0xF19F,0xD5BF,0xB5B4,0x180B,0x4933,0x3414,0x5DDF},
 {0x0000,0x1082,0x39C7,0x7BEF,0xBDF7,0x8410,0x0801,0x3186,0x2104,0x5DDF},
 {0x1008,0x280C,0xA01F,0xF81F,0xD7FF,0xB5B4,0x180B,0x4933,0x3414,0x07FF},
 {0x0000,0x0100,0x03E0,0x07E0,0xBFE0,0x87F0,0x0020,0x03E0,0x0200,0x07E0},
 {0x2104,0x4208,0xFC00,0xFD20,0xFFE0,0xC618,0x2945,0x7BE0,0x6300,0xFEA0},
 {0x0000,0x2104,0x7BEF,0xFFFF,0xE71C,0xA514,0x1082,0x6318,0x3186,0xC618}
};
void applyTheme(uint8_t index){
  themeMode=index%6;const uint16_t* p=themePalette[themeMode];
  BG=p[0];PANEL=p[1];UI_PURPLE=p[2];UI_PINK=p[3];UI_LAVENDER=p[4];UI_MUTED=p[5];
  UI_GRID=p[6];UI_EDGE=p[7];UI_PANEL_HI=p[8];UI_CYAN=p[9];
}
void drawThemeScreen(){
  header("THEME SETTINGS");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_MUTED,BG);d.setCursor(8,44);d.print("PREVIEW / SELECT WITH LEFT OR RIGHT");
  d.fillRoundRect(8,57,224,43,5,UI_PANEL_HI);d.drawRoundRect(8,57,224,43,5,UI_PINK);
  d.setTextSize(2);d.setTextColor(UI_LAVENDER,BG);d.setCursor(17,68);d.print(themeNames[themeSelection]);d.setTextSize(1);
  d.setTextColor(UI_CYAN,BG);d.setCursor(18,88);d.print("JOHNNY // COLOR PROFILE");
  for(int i=0;i<6;i++)d.fillRoundRect(16+i*35,106,27,7,2,themePalette[i][3]);
  footer(", / PREVIEW   ENTER SAVE   ESC BACK");
}
void beginClockNtp(){
  if(WiFi.status()!=WL_CONNECTED)return;
  configTime(0,0,"pool.ntp.org","time.google.com","time.cloudflare.com");
  // Embedded newlib uses POSIX TZ strings; BRT3 is the current Sao Paulo UTC-3 rule.
  setenv("TZ","BRT3",1);tzset();
  clockNtpStarted=true;
}
void persistAlarm(){
  preferences.putBool("alarm_on",alarmEnabled);preferences.putUChar("alarm_h",alarmHour);
  preferences.putUChar("alarm_m",alarmMinute);preferences.putUChar("alarm_days",alarmWeekdayMask);
  preferences.putString("alarm_mp3",alarmSoundPath);
}
void drawClockScreen(){
  header("CLOCK / ALARMS");auto& d=M5Cardputer.Display;
  time_t now=time(nullptr);struct tm t{};bool valid=now>1600000000&&localtime_r(&now,&t)!=nullptr;
  d.fillRoundRect(8,52,224,37,5,UI_PANEL_HI);
  d.setTextSize(3);d.setTextColor(valid?WHITE:UI_MUTED,BG);d.setCursor(22,57);
  if(valid)d.printf("%02d:%02d:%02d",t.tm_hour,t.tm_min,t.tm_sec);else d.print("--:--:--");
  d.setTextSize(1);d.setTextColor(UI_LAVENDER,BG);d.setCursor(13,94);
  if(valid)d.printf("%02d/%02d/%04d  SAO PAULO",t.tm_mday,t.tm_mon+1,t.tm_year+1900);
  else d.print(WiFi.status()==WL_CONNECTED?"WAITING FOR NTP SYNC":"NO CLOCK SYNC // CONNECT WI-FI");
  d.fillRoundRect(8,103,224,11,3,0x2010);
  d.setTextColor(alarmEnabled?UI_PINK:UI_MUTED,0x2010);d.setCursor(13,105);
  d.printf("ALARM %s  %02u:%02u  %s",alarmEnabled?"ON":"OFF",alarmHour,alarmMinute,alarmSoundPath.length()?"CUSTOM MP3":"BUILT-IN TONE");
  if(alarmActive){d.fillRoundRect(8,39,224,12,3,UI_PINK);d.setTextColor(BLACK,UI_PINK);d.setCursor(82,42);d.print("ALARM RINGING");}
  footer("T SYNC  A ON/OFF  ; . HOUR  , / MIN  D DAYS  ENTER STOP  S SNOOZE");
}
void drawDiskManager(){
  header("DISK MANAGER");auto& d=M5Cardputer.Display;
  uint32_t flash=ESP.getFlashChipSize(),sketch=ESP.getSketchSize(),freeSketch=ESP.getFreeSketchSpace();
  d.setTextColor(UI_CYAN,BG);d.setCursor(8,43);d.print("INTERNAL FLASH");
  d.setTextColor(WHITE,BG);d.setCursor(8,55);d.printf("PHYSICAL %.2f MB",flash/1048576.0f);
  d.setCursor(8,67);d.printf("APP IMAGE %.2f MB",sketch/1048576.0f);
  d.setCursor(8,79);d.printf("OTA SLOT %.2f MB FREE",freeSketch/1048576.0f);
  d.setTextColor(UI_CYAN,BG);d.setCursor(8,94);d.print("MICROSD");
  if(sdReady){
    uint64_t total=SD.totalBytes(),used=SD.usedBytes(),freeBytes=total>used?total-used:0;
    d.setTextColor(WHITE,BG);d.setCursor(8,106);d.printf("%.1f / %.1f MB USED",used/1048576.0,total/1048576.0);
    d.setCursor(8,117);d.printf("%.1f MB FREE",freeBytes/1048576.0);
  }else{d.setTextColor(UI_PINK,BG);d.setCursor(8,107);d.print("CARD NOT MOUNTED");}
  footer("CAPACITY VIEW / SAFE MODE   R REFRESH");
}
void drawProcessInfo(){
  header("PROCESS MANAGER");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_CYAN,BG);d.setCursor(8,43);d.print("RUNTIME / HEAP");
  d.setTextColor(WHITE,BG);d.setCursor(8,55);d.printf("FREE HEAP   %lu KB",ESP.getFreeHeap()/1024UL);
  d.setCursor(8,66);d.printf("LARGEST BLK %lu KB",ESP.getMaxAllocHeap()/1024UL);
  d.setCursor(8,77);d.printf("TASKS       %u",uxTaskGetNumberOfTasks());
  d.setTextColor(UI_CYAN,BG);d.setCursor(8,91);d.print("JOHNNY TASKS");
  TaskHandle_t tasks[2]={johnnyAudioTaskHandle,johnnyRecorderTaskHandle};
  const char* labels[2]={"AUDIO DECODER","RADIO RECORDER"};
  for(int i=0;i<2;i++){int y=103+i*11;if(!tasks[i])continue;d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,y);d.print(labels[i]);d.setTextColor(UI_MUTED,BG);d.setCursor(126,y);d.printf("STACK %u B",(unsigned)(uxTaskGetStackHighWaterMark(tasks[i])*sizeof(StackType_t)));}
  footer("MANAGED TASKS ONLY / NO FORCE KILL");
}
void drawChargingSaver(){
  auto& d=M5Cardputer.Display;d.fillScreen(BLACK);int pct=batteryPercent();
  d.drawRoundRect(53,34,134,57,10,UI_LAVENDER);d.fillRect(187,51,6,22,UI_LAVENDER);
  if(pct>=0){int w=constrain(pct,0,100)*116/100;if(w>0)d.fillRoundRect(61,42,w,41,5,UI_PINK);}
  d.setTextSize(3);d.setTextColor(WHITE,BLACK);d.setCursor(75,53);if(pct>=0)d.printf("%d%%",pct);else d.print("--%");
  d.setTextSize(1);d.setTextColor(UI_CYAN,BLACK);d.setCursor(91,104);d.print("CHARGING // JOHNNY");
  d.setTextColor(UI_MUTED,BLACK);d.setCursor(67,119);d.print("PRESS ANY KEY TO EXIT");
}
void silenceAlarm(bool snooze){
  if(snooze){alarmSnoozeUntilMs=millis()+300000UL;time_t now=time(nullptr);if(now>1600000000)alarmSnoozeUntilMs=0,alarmSnoozeUntilEpoch=(uint64_t)now+300U;}
  else{alarmSnoozeUntilMs=0;alarmSnoozeUntilEpoch=0;}
  alarmActive=false;alarmTriedFile=false;alarmSoundStarted=false;M5Cardputer.Speaker.stop(0);
  if(audioStarted||audioStopRequested)stopAudio();
  if(screen==CLOCKSCREEN)drawClockScreen();
}
void serviceClockAlarm(){
  if(WiFi.status()==WL_CONNECTED&&!clockNtpStarted)beginClockNtp();
  time_t now=time(nullptr);struct tm t{};bool valid=now>1600000000&&localtime_r(&now,&t)!=nullptr;
  if(!alarmActive&&alarmEnabled){
    bool snoozeDue=alarmSnoozeUntilEpoch?(uint64_t)now>=alarmSnoozeUntilEpoch:(alarmSnoozeUntilMs&&(int32_t)(millis()-alarmSnoozeUntilMs)>=0);
    int32_t dayKey=valid?(t.tm_year+1900)*10000+(t.tm_mon+1)*100+t.tm_mday:-1;
    bool due=valid&&(alarmWeekdayMask&(1U<<t.tm_wday))&&t.tm_hour==alarmHour&&t.tm_min==alarmMinute&&dayKey!=alarmLastDayKey;
    if(snoozeDue||due){
      alarmActive=true;alarmTriedFile=false;alarmSoundStarted=false;alarmLastDayKey=dayKey;
      alarmSnoozeUntilEpoch=0;alarmSnoozeUntilMs=0;stopAudio();M5Cardputer.Speaker.stop(SYNTH_AUDIO_CHANNEL);
      if(micRecording)stopMicRecording();if(radioRecordState==1||radioRecordState==2)stopRadioRecording();screen=CLOCKSCREEN;lastClockDraw=0;drawClockScreen();
    }
  }
  if(!alarmActive)return;
  if(!alarmTriedFile){
    if(audioStarted||audioStopRequested)return;
    alarmTriedFile=true;
    if(alarmSoundPath.length()&&sdReady&&SD.exists(alarmSoundPath)&&audioExtension(alarmSoundPath)=="mp3")alarmSoundStarted=startLocalAudio(alarmSoundPath);
  }
  if(alarmSoundStarted&&!audioStarted&&!audioStopRequested){alarmSoundStarted=false;alarmTriedFile=false;lastAlarmToneMs=millis()-900;}
  if(!alarmSoundStarted&&!audioStarted&&(uint32_t)(millis()-lastAlarmToneMs)>=900UL){
    lastAlarmToneMs=millis();
    uint32_t phase=0,step=(uint32_t)(880.0*(4294967296.0/16000.0));
    for(size_t i=0;i<512;i++){float env=(i<28)?(float)i/28.0f:((i>468)?(float)(511-i)/43.0f:1.0f);alarmToneSamples[i]=(int16_t)(sinf(6.28318530718f*(float)i*880.0f/16000.0f)*env*8500.0f);phase+=step;}
    M5Cardputer.Speaker.playRaw(alarmToneSamples,512,16000,false,1,0,false);
  }
}

void setup() {
  auto cfg=M5.config();
  M5Cardputer.begin(cfg,true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(118);playBootJingle();
  audioMutex=xSemaphoreCreateMutex();
  if(!audioMutex) Serial.println("JOHNNY: AUDIO MUTEX FAILED");
  pinMode(38,OUTPUT); digitalWrite(38,HIGH);
  randomSeed(esp_random());
  sdBus.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  sdReady=SD.begin(SD_CS,sdBus,20000000);
  Serial.begin(115200);
  preferences.begin("johnny",false);
  savedSsid=preferences.getString("ssid","");
  savedPassword=preferences.getString("pass","");
  themeMode=preferences.getUChar("theme",0);applyTheme(themeMode);
  alarmEnabled=preferences.getBool("alarm_on",false);alarmHour=preferences.getUChar("alarm_h",7);alarmMinute=preferences.getUChar("alarm_m",0);alarmWeekdayMask=preferences.getUChar("alarm_days",0x7F);alarmSoundPath=preferences.getString("alarm_mp3","");
  radioEntry=preferences.getString("radio_url","");
  eqPreset=preferences.getInt("eqpreset",0);if(eqPreset<0||eqPreset>3)eqPreset=-1;
  for(int i=0;i<(int)JOHNNY_EQ_BANDS;i++){
    float fallback=eqPreset>=0?eqPresets[eqPreset][i]:eqPresets[0][i];
    audioEqGainDb[i]=preferences.getFloat((String("eq")+String(i)).c_str(),fallback);
  }
  audioEqualizer.setAll(audioEqGainDb);
  // Wi-Fi starts only when requested from the menu; keep cold boot lean.
  auto micCfg=M5Cardputer.Mic.config();
  micCfg.sample_rate=16000;
  M5Cardputer.Mic.config(micCfg);
  refreshBatteryCache(true);
  // Audio shares core 1 with the UI at equal priority and yields every 1 ms.
  // This keeps playback smooth without allowing the decoder to freeze menus.
  if(audioMutex)xTaskCreatePinnedToCore(audioTask,"JOHNNY_AUDIO",10240,nullptr,1,&johnnyAudioTaskHandle,0);
  radioRecordQueue=xQueueCreate(8,sizeof(RadioRecordBlock));
  if(radioRecordQueue)xTaskCreatePinnedToCore(radioRecorderTask,"JOHNNY_REC",4096,nullptr,2,&johnnyRecorderTaskHandle,0);
  splash();selected=0;drawHome();
}
void loop() {
  M5Cardputer.update();
  if(millis()-batteryLastSample>=2000UL) {
    batteryLastDraw=millis();
    refreshBatteryCache(true);
    if(screen!=USBMODE && !screenSaverActive) { drawBatteryIndicator(); if(screen==ABOUT)drawDeviceStatus(); }
  }
  serviceAudio();
  serviceClockAlarm();
  serviceMusicProgress();
  if(screen==CLOCKSCREEN&&!screenSaverActive&&(uint32_t)(millis()-lastClockDraw)>=1000UL){lastClockDraw=millis();drawClockScreen();}
  if(batteryCharging()){
    if(!chargingDetectedAt)chargingDetectedAt=millis();
    if(!chargingSaverDismissed&&!chargingSaverActive&&(uint32_t)(millis()-chargingDetectedAt)>=2000UL&&screen!=USBMODE){chargingSaverActive=true;screenSaverActive=true;drawChargingSaver();}
  }else{
    chargingDetectedAt=0;chargingSaverDismissed=false;
    if(chargingSaverActive){chargingSaverActive=false;screenSaverActive=false;if(screen==HOME)drawHome();else if(screen==CLOCKSCREEN)drawClockScreen();else if(screen==MUSIC&&musicNowPlaying)drawMusicInfo(audioNowTitle);else if(screen==RADIO)drawRadioScreen();else if(screen==PIANO)pianoScreen();else if(screen==MICREC)drawMicRecorder();else if(screen==FILES||screen==MUSIC)drawFiles(screen==MUSIC);else if(screen==NETWORK)drawNetworkScreen();else drawCategoryMenu();}
  }
  if((screen==MICREC||screen==PIANO)&&micRecording)serviceMicRecorder();
  if(!screenSaverActive&&screen==LIFE&&lifeRunning&&millis()-lifeLastStep>=lifeIntervalMs){stepLife();drawLife();}
  static uint8_t lastRecorderDisplayState=255;
  if(screen==RADIO&&!screenSaverActive&&lastRecorderDisplayState!=radioRecordState){lastRecorderDisplayState=radioRecordState;drawRadioScreen();}
  if(screen==ABOUT&&!screenSaverActive){int status=(int)WiFi.status();if(status!=lastAboutWiFiStatus){lastAboutWiFiStatus=status;drawDeviceStatus();}}
  if(!screenSaverActive && screen==NETWORK && wifiConfigStage==0) {
    int status=(int)WiFi.status();
    if(status!=lastWiFiStatus) { lastWiFiStatus=status; drawNetworkScreen(); }
  }
  if(!screenSaverActive && screen==NETWORK && wifiConfigStage==1 && wifiScanPending) {
    int result=WiFi.scanComplete();
    if(result>=0) {
      wifiNetworkCount=min(result,20);
      for(int i=0;i<wifiNetworkCount;i++) { wifiNetworks[i]=WiFi.SSID(i); wifiNetworkRSSI[i]=WiFi.RSSI(i); wifiNetworkAuth[i]=(int)WiFi.encryptionType(i);wifiNetworkChannel[i]=WiFi.channel(i);wifiNetworkBssid[i]=WiFi.BSSIDstr(i); }
      wifiScanPending=false; WiFi.scanDelete();
      if(wifiSelected>=wifiNetworkCount) wifiSelected=max(0,wifiNetworkCount-1);
      drawNetworkScreen();
    } else if(result==WIFI_SCAN_FAILED) { wifiScanPending=false; drawNetworkScreen(); }
  }
  if(screen==TUNER && !screenSaverActive && millis()-tunerLastSample>240) {
    tunerLastSample=millis();
    if(captureMic(tunerSamples,3072,420)) {
      float freq=detectPitch(tunerSamples,3072);
      if(freq>=36.0f && freq<=560.0f) {
        if(tunerFilteredFrequency<=0.0f || fabsf(freq-tunerFilteredFrequency)>tunerFilteredFrequency*0.30f) tunerFilteredFrequency=freq;
        else tunerFilteredFrequency=0.72f*tunerFilteredFrequency+0.28f*freq;
        tunerFrequency=tunerFilteredFrequency; tunerHasSignal=true; tunerLastSignalAt=millis();
      }
    }
    if(tunerHasSignal && millis()-tunerLastSignalAt>1100UL) {tunerHasSignal=false;tunerFilteredFrequency=0;tunerFrequency=0;}
    drawTunerReadout();
  }
  
  if(Serial.available()>0) serialTransferTask();
  serviceWebServer();
  serviceBleScan();
  if(!screenSaverActive&&screen==HACKFX&&hackVisualFx&&millis()-hackFxLastFrame>=120){hackFxLastFrame=millis();M5Cardputer.Display.fillScreen((random(8)==0)?UI_PURPLE:BG);M5Cardputer.Display.setTextColor(UI_PINK,BG);M5Cardputer.Display.setCursor(random(8,160),random(35,110));M5Cardputer.Display.print("NO RF TX // OFFLINE");}

  if(!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) { delay(4); return; }
  auto k=M5Cardputer.Keyboard.keysState();
  char c=k.word.size()?k.word[0]:0;
  if(k.tab) {
    screenSaverActive=!screenSaverActive;
    if(screenSaverActive) M5Cardputer.Display.fillScreen(BLACK);
    else if(screen==HOME) drawHome();
    else if(screen==MUSICMENU||screen==SYSTEMMENU||screen==GAMESMENU||screen==HACKMENU) drawCategoryMenu();
    else if(screen==UPDATEINFO) drawUpdateInfo();
    else if(screen==MICREC) drawMicRecorder();
    else if(screen==PIANO) pianoScreen();
    else if(screen==TUNER) tunerScreen();
    else if(screen==FILES) drawFiles(false);
    else if(screen==MUSIC) musicNowPlaying?drawMusicInfo(audioNowTitle):drawFiles(true);
    else if(screen==RADIO) drawRadioScreen();
    else if(screen==NETWORK) drawNetworkScreen();
    else if(screen==USBMODE)drawUsbScreen(usbActive);
    else if(screen==EQSCREEN)drawEqScreen();
    else if(screen==LIFE)drawLife();
    else if(screen==ABOUT)openApp();
    else if(screen==BLESCAN)drawBleScreen();
    else if(screen==HACKFX)drawHackFxScreen();
    else if(screen==HACKINFO)drawHackInfo();
    else if(screen==HACKINPUT)drawHackInput();
    else if(screen==CLOCKSCREEN)drawClockScreen();else if(screen==DISKMANAGER)drawDiskManager();else if(screen==PROCESSINFO)drawProcessInfo();else if(screen==THEMES)drawThemeScreen();
    if(chargingSaverActive){chargingSaverActive=false;chargingSaverDismissed=true;}
    return;
  }
  if(screenSaverActive) {
    screenSaverActive=false;
    if(screen==HOME) drawHome();
    else if(screen==MUSICMENU||screen==SYSTEMMENU||screen==GAMESMENU||screen==HACKMENU) drawCategoryMenu();
    else if(screen==UPDATEINFO) drawUpdateInfo();
    else if(screen==MICREC) drawMicRecorder();
    else if(screen==PIANO) pianoScreen();
    else if(screen==TUNER) tunerScreen();
    else if(screen==FILES) drawFiles(false);
    else if(screen==MUSIC) musicNowPlaying?drawMusicInfo(audioNowTitle):drawFiles(true);
    else if(screen==RADIO) drawRadioScreen();
    else if(screen==NETWORK) drawNetworkScreen();
    else if(screen==USBMODE)drawUsbScreen(usbActive);
    else if(screen==EQSCREEN)drawEqScreen();
    else if(screen==LIFE)drawLife();
    else if(screen==ABOUT)openApp();
    else if(screen==BLESCAN)drawBleScreen();
    else if(screen==HACKFX)drawHackFxScreen();
    else if(screen==HACKINFO)drawHackInfo();
    else if(screen==HACKINPUT)drawHackInput();
    else if(screen==CLOCKSCREEN)drawClockScreen();else if(screen==DISKMANAGER)drawDiskManager();else if(screen==PROCESSINFO)drawProcessInfo();else if(screen==THEMES)drawThemeScreen();
    if(chargingSaverActive){chargingSaverActive=false;chargingSaverDismissed=true;}
    return;
  }

  if(alarmActive){if(k.enter||c=='s'||c=='S'){silenceAlarm(c=='s'||c=='S');return;}return;}
  bool back=(c==0x1B || c=='`' || c=='~' || (k.del && screen!=RADIO && screen!=HACKINPUT && screen!=NETWORK));
  if(back) {
    if(screen==MICREC){stopMicRecording();screen=appReturnMenu;selected=appReturnSelected;drawCategoryMenu();}
    else if(screen==THEMES){applyTheme(themeMode);screen=appReturnMenu;selected=appReturnSelected;drawCategoryMenu();}
    else if(screen==UPDATEINFO){screen=SYSTEMMENU;selected=3;drawCategoryMenu();}
    else if(screen==MUSICMENU||screen==SYSTEMMENU||screen==GAMESMENU||screen==HACKMENU){screen=HOME;selected=0;drawHome();}
    else if(screen==BLESCAN){stopBleScan();screen=appReturnMenu;selected=appReturnSelected;drawCategoryMenu();}
    else if(screen==HACKINFO){screen=HACKMENU;selected=appReturnSelected;drawCategoryMenu();}
    else if(screen==HACKINPUT){screen=HACKMENU;selected=appReturnSelected;drawCategoryMenu();}
    else if(screen==HACKFX){hackVisualFx=false;screen=HACKMENU;selected=6;drawCategoryMenu();}
    else if(screen==NETWORK&&wifiScanOnly){if(wifiSurveyDetails){wifiSurveyDetails=false;drawNetworkScreen();}else{wifiScanOnly=false;wifiConfigStage=0;screen=HACKMENU;selected=0;drawCategoryMenu();}}
    else if(screen==EQSCREEN){
      screen=eqReturnScreen;
      if(screen==HOME)drawHome();
      else if(screen==MUSICMENU)drawCategoryMenu();
      else if(screen==PIANO)pianoScreen();
      else if(screen==MUSIC)musicNowPlaying?drawMusicInfo(audioNowTitle):drawFiles(true);
      else if(screen==RADIO)drawRadioScreen();
    }else if(screen==LIFE){saveLife();screen=appReturnMenu;selected=appReturnSelected;drawCategoryMenu();}
    else if(screen==FILES || screen==MUSIC) {
      bool music=screen==MUSIC;
      if(music && musicNowPlaying) {if(stopAudioAndWait(650)){musicNowPlaying=false;audioStatusMessage="STOPPED";drawFiles(true);}else{audioStatusMessage="AUDIO BUSY TIMEOUT";drawMusicInfo(audioNowTitle);}}
      else if(currentPath!="/") {currentPath=parentPath(currentPath);scanDirectory(music);drawFiles(music);}
      else {screen=appReturnMenu;selected=appReturnSelected;drawCategoryMenu();}
    } else if(screen==RADIO) {
      if(radioEditingUrl) {radioEditingUrl=false;drawRadioScreen();}
      else {screen=appReturnMenu;selected=appReturnSelected;drawCategoryMenu();}
    } else if(screen==USBMODE) {
if(usbActive) endUsbMode();
      screen=appReturnMenu;selected=appReturnSelected;drawCategoryMenu();
    } else {
      if(screen==PIANO){if(micRecording)stopMicRecording();M5Cardputer.Speaker.stop(SYNTH_AUDIO_CHANNEL);}
      if(screen==TUNER) stopMicCapture();
      if(screen==NETWORK && wifiConfigStage==2) { wifiConfigStage=1; wifiEntry=""; drawNetworkScreen(); }
      else { wifiConfigStage=0;screen=appReturnMenu;selected=appReturnSelected;drawCategoryMenu(); }
    }
    delay(8); return;
  }

  if(screen==MICREC) {
    if(k.enter||k.space){if(micRecording)stopMicRecording();else startMicRecording();}
  } else if(screen==CLOCKSCREEN) {
    if(alarmActive&&(k.enter||c=='s'||c=='S'))silenceAlarm(c=='s'||c=='S');
    else if(c=='t'||c=='T'){beginClockNtp();drawClockScreen();}
    else if(c=='a'||c=='A'){alarmEnabled=!alarmEnabled;persistAlarm();drawClockScreen();}
    else if(c==';'){alarmHour=(alarmHour+23)%24;persistAlarm();drawClockScreen();}
    else if(c=='.'){alarmHour=(alarmHour+1)%24;persistAlarm();drawClockScreen();}
    else if(c==','){alarmMinute=(alarmMinute+59)%60;persistAlarm();drawClockScreen();}
    else if(c=='/'){alarmMinute=(alarmMinute+1)%60;persistAlarm();drawClockScreen();}
    else if(c=='d'||c=='D'){alarmWeekdayMask=alarmWeekdayMask==0x7F?0x3E:(alarmWeekdayMask==0x3E?0x41:0x7F);persistAlarm();drawClockScreen();}
    else if(c=='m'||c=='M'){alarmSoundPath="";persistAlarm();drawClockScreen();}
  } else if(screen==DISKMANAGER) {
    if(c=='r'||c=='R')drawDiskManager();
  } else if(screen==PROCESSINFO) {
    if(c=='r'||c=='R')drawProcessInfo();
  } else if(screen==THEMES) {
    if(c==','){themeSelection=(themeSelection+5)%6;applyTheme(themeSelection);drawThemeScreen();}
    else if(c=='/'){themeSelection=(themeSelection+1)%6;applyTheme(themeSelection);drawThemeScreen();}
    else if(k.enter){themeMode=themeSelection;preferences.putUChar("theme",themeMode);applyTheme(themeMode);screen=appReturnMenu;selected=appReturnSelected;drawCategoryMenu();}
  } else if(screen==UPDATEINFO) {
    if(k.enter) runFirmwareUpdate();
  } else if(screen==MUSICMENU||screen==SYSTEMMENU||screen==GAMESMENU||screen==HACKMENU) {
    int count=screen==MUSICMENU?MUSIC_MENU_COUNT:(screen==SYSTEMMENU?SYSTEM_MENU_COUNT:(screen==GAMESMENU?GAMES_MENU_COUNT:HACK_MENU_COUNT));
    if(k.enter){openApp();delay(8);return;}
    if(c==','){if(selected%2==1)selected--;}
    else if(c=='/'){if(selected%2==0&&selected+1<count)selected++;}
    else if(c==';')selected=max(0,selected-2);
    else if(c=='.')selected=min(count-1,selected+2);
    drawCategoryMenu();
  } else if(screen==HOME) {
    if(k.enter) { openApp(); delay(8); return; }
    if(c==',')selected=(selected+APP_COUNT-1)%APP_COUNT;
    else if(c=='/')selected=(selected+1)%APP_COUNT;
    drawHome();
  } else if(screen==PIANO) {
    if(c==','){pianoOctave=max(-2,pianoOctave-1);pianoScreen();}
    else if(c=='/'){pianoOctave=min(2,pianoOctave+1);pianoScreen();}
    else if(c==';'){pianoEffect=(pianoEffect+7)%8;pianoScreen();}
    else if(c=='.'){pianoEffect=(pianoEffect+1)%8;pianoScreen();}
    else if(c=='-'){pianoVolume=max(0,pianoVolume-10);pianoScreen();}
    else if(c=='='||c=='+'){pianoVolume=min(100,pianoVolume+10);pianoScreen();}
    else if(c=='['){synthPadSelected=(synthPadSelected+6)%7;pianoScreen();}
    else if(c==']'){synthPadSelected=(synthPadSelected+1)%7;pianoScreen();}
    else if(c=='r'||c=='R'){if(micRecording)stopMicRecording();else startMicRecording();}
    else if(c)playNote(c);
  } else if(screen==TUNER) {
    if(c==','||c=='/') {tunerInstrument=(tunerInstrument+1)%3;tunerScreen();}
  } else if(screen==MUSIC) {
    if(musicNowPlaying) {
      if(c==',')moveMusicTrack(-1);
      else if(c=='/') moveMusicTrack(1);
      else if(k.enter||k.space||c==' ') {toggleAudioPause();drawMusicInfo(audioNowTitle);}
      else if(c=='p'||c=='P') {stopAudio();musicNowPlaying=false;audioStatusMessage="STOPPED";drawFiles(true);}
      else if(c=='-'||c=='_') {setAudioVolume(audioVolume-1);drawMusicInfo(audioNowTitle);}
      else if(c=='='||c=='+') {setAudioVolume(audioVolume+1);drawMusicInfo(audioNowTitle);}
      else if(c==';') {seekAudioStep(-1);drawMusicInfo(audioTrackTitle.length()?audioTrackTitle:audioNowTitle);}
      else if(c=='.') {seekAudioStep(1);drawMusicInfo(audioTrackTitle.length()?audioTrackTitle:audioNowTitle);}
      else if(c=='s'||c=='S'){audioShuffle=!audioShuffle;drawMusicInfo(audioNowTitle);}
      else if(c=='r'||c=='R'){audioRepeatMode=(audioRepeatMode+1)%3;drawMusicInfo(audioNowTitle);}
      else if(c=='a'||c=='A'){if(audioTrackExtension=="mp3"&&audioTrackPath.length()){alarmSoundPath=audioTrackPath;persistAlarm();audioStatusMessage="ALARM MP3 SAVED";drawMusicInfo(audioNowTitle);}else{audioStatusMessage="SELECT MP3 FIRST";drawMusicInfo(audioNowTitle);}}
    } else {      if(c==';') {if(selected>0)selected--;if(selected<scroll)scroll=selected;drawFiles(true);}
      else if(c=='.') {if(selected<entryCount-1)selected++;if(selected>=scroll+5)scroll=selected-4;drawFiles(true);}      else if(c==',') {if(currentPath!="/"){currentPath=parentPath(currentPath);scanDirectory(true);drawFiles(true);}}
      else if(k.enter) openFile(true);
    }
  } else if(screen==FILES) {
    if(c==';') {if(selected>0)selected--;if(selected<scroll)scroll=selected;drawFiles(false);}
    else if(c=='.') {if(selected<entryCount-1)selected++;if(selected>=scroll+5)scroll=selected-4;drawFiles(false);}
    else if(k.enter) openFile(false);
  } else if(screen==RADIO) {
    if(radioEditingUrl) {
      if(k.enter) {
        radioEntry.trim();
        if(radioEntry.startsWith("http://")) {
          preferences.putString("radio_url",radioEntry); radioEditingUrl=false;
          radioMessage="CONNECTING...";
          drawRadioScreen();
          startRadioStream(radioEntry,"CUSTOM STREAM");
          drawRadioScreen();
        } else {radioMessage="URL MUST START HTTP://";drawRadioScreen();}
      } else if(c==8||c==127) {if(radioEntry.length())radioEntry.remove(radioEntry.length()-1);drawRadioScreen();}
      else if(c&&isprint((unsigned char)c)&&radioEntry.length()<180) {radioEntry+=c;drawRadioScreen();}
    } else {
      if(c=='r'||c=='R'){
        if(radioRecordState==1||radioRecordState==2)stopRadioRecording();else startRadioRecording();
        drawRadioScreen();
      }
      else if(c==';'){radioSelected=max(0,radioSelected-1);drawRadioScreen();}
      else if(c=='.') {radioSelected=min(RADIO_STATION_COUNT,radioSelected+1);drawRadioScreen();}
      else if(k.enter) {
        if(radioSelected<RADIO_STATION_COUNT) {
          radioMessage="CONNECTING...";
          drawRadioScreen();
          startRadioStream(radioStationUrls[radioSelected],radioStationNames[radioSelected]);
        } else {
          radioEntry=preferences.getString("radio_url",radioEntry);
          radioEditingUrl=true;
        }
        drawRadioScreen();
      } else if(k.space&&audioIsRadio) {toggleAudioPause();drawRadioScreen();}
      else if(c=='p'||c=='P') {stopAudio();audioStatusMessage="STOPPED";radioMessage="STOPPED";drawRadioScreen();}
      else if(c=='-'||c=='_') {setAudioVolume(audioVolume-1);drawRadioScreen();}
      else if(c=='='||c=='+') {setAudioVolume(audioVolume+1);drawRadioScreen();}
    }

  } else if(screen==EQSCREEN) {
    if(c==';'){eqSelectedBand=max(0,eqSelectedBand-1);drawEqScreen();}
    else if(c=='.'){eqSelectedBand=min((int)JOHNNY_EQ_BANDS-1,eqSelectedBand+1);drawEqScreen();}
    else if(c==','){setEqBandGain(eqSelectedBand,audioEqGainDb[eqSelectedBand]-0.5f);drawEqScreen();}
    else if(c=='/'){setEqBandGain(eqSelectedBand,audioEqGainDb[eqSelectedBand]+0.5f);drawEqScreen();}
    else if(c=='-'||c=='_'){setEqBandGain(eqSelectedBand,audioEqGainDb[eqSelectedBand]-0.5f);drawEqScreen();}
    else if(c=='='||c=='+'){setEqBandGain(eqSelectedBand,audioEqGainDb[eqSelectedBand]+0.5f);drawEqScreen();}
    else if(c=='p'||c=='P'){applyEqPreset(eqPreset<0?0:(eqPreset+1)%4);drawEqScreen();}
    else if(c=='0'){applyEqPreset(0);drawEqScreen();}
  } else if(screen==LIFE) {
    if(c==' '||k.space){lifeRunning=!lifeRunning;lifeLastStep=millis();drawLife();}
    else if(c=='n'||c=='N'){lifeRunning=false;stepLife();drawLife();}
    else if(c=='r'||c=='R'){seedLife();drawLife();}
    else if(c=='c'||c=='C'){clearLife();drawLife();}
    else if(c==';'){lifeCursorY=max(0,lifeCursorY-1);drawLife();}
    else if(c=='.'){lifeCursorY=min(LIFE_ROWS-1,lifeCursorY+1);drawLife();}
    else if(c==','){lifeCursorX=max(0,lifeCursorX-1);drawLife();}
    else if(c=='/'){lifeCursorX=min(LIFE_COLS-1,lifeCursorX+1);drawLife();}
    else if(k.enter){toggleLifeCell();drawLife();}
    else if(c=='-'||c=='_'){lifeIntervalMs=min(1000,(int)lifeIntervalMs+50);drawLife();}
    else if(c=='='||c=='+'){lifeIntervalMs=max(50,(int)lifeIntervalMs-50);drawLife();}
  } else if(screen==BLESCAN) {
    if(c=='r'||c=='R'){if(bleScanning&&bleScanner)bleScanner->stop();startBleScan();drawBleScreen();}
  } else if(screen==HACKINPUT) {
    if(hackInputDone){if(c=='r'||c=='R'){hackInputDone=false;hackInputValue="";hackResultMessage="";drawHackInput();}}
    else if(k.enter)runHackInput();
    else if(c==8||c==127){if(hackInputValue.length())hackInputValue.remove(hackInputValue.length()-1);drawHackInput();}
    else if(c&&isprint((unsigned char)c)&&hackInputValue.length()<100){hackInputValue+=c;drawHackInput();}
  } else if(screen==HACKINFO) {
    // ESC returns to the HACK menu.
  } else if(screen==HACKFX) {
    // Visual-only demo; ESC returns to the HACK menu.
  } else if(screen==NETWORK) {
    if(wifiConfigStage==0) {
      if(k.enter) startNetworkSetup();
      else if(c=='.') {WiFi.disconnect(false,false);drawNetworkScreen();}
    } else if(wifiConfigStage==1) {
      if(c=='r'||c=='R') {if(wifiScanOnly)startWifiSurvey();else startNetworkSetup();}
      else if(c==';') {if(wifiSelected>0)wifiSelected--;if(wifiSelected<wifiScroll)wifiScroll=wifiSelected;drawNetworkScreen();}
      else if(c=='.') {if(wifiSelected<wifiNetworkCount-1)wifiSelected++;if(wifiSelected>=wifiScroll+5)wifiScroll=wifiSelected-4;drawNetworkScreen();}
      else if(k.enter && !wifiScanPending) {if(wifiScanOnly){wifiSurveyDetails=true;drawNetworkScreen();}else chooseNetwork(wifiSelected);}
    } else if(wifiConfigStage==2) {
      if(k.enter) saveNetworkSetup();
      else if(c && isprint((unsigned char)c) && wifiEntry.length()<63) {wifiEntry+=c;drawNetworkScreen();}
    }
  } else if(screen==USBMODE) {
    // USB transfer is serviced through the serial protocol above.
  }
}