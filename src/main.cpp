#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <WiFi.h>
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
#include <AudioGeneratorWAV.h>
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorAAC.h>
#include <AudioGeneratorOpus.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/queue.h>
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
    uint32_t started=millis();
    while(speaker_->isPlaying() && (uint32_t)(millis()-started)<1000UL) delay(1);
    if(speaker_->isPlaying()) speaker_->stop();
    speaker_->playRaw(buffers_[bufferSlot_],bufferIndex_,hertz,false,1,0,true);
    bufferSlot_=(bufferSlot_+1)%3;bufferIndex_=0;
  }
  void flushRecording(){if(recordIndex_>0)sendRecordBlock();}
  void resetRecordingBuffer(){recordIndex_=0;}
  bool stop() override {
    flush();
    if(speaker_) {
      uint32_t started=millis();
      while(speaker_->isPlaying() && (uint32_t)(millis()-started)<1000UL) delay(1);
      if(speaker_->isPlaying()) speaker_->stop();
    }
    return true;
  }
private:
  static constexpr size_t kBufferSamples=1536;
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
constexpr uint16_t BG = 0x1008, PANEL = 0x280C, UI_PURPLE = 0xA01F, UI_PINK = 0xF19F, UI_LAVENDER = 0xD5BF, UI_MUTED = 0xB5B4;
enum Screen : uint8_t { HOME, MUSICMENU, SYSTEMMENU, GAMESMENU, HACKMENU, HACKFX, BLESCAN, HACKINFO, PIANO, TUNER, FILES, MUSIC, RADIO, NETWORK, USBMODE, EQSCREEN, LIFE, ABOUT, UPDATEINFO, MICREC };
Screen eqReturnScreen=HOME;
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
String entries[48];
bool entryIsDir[48];
int entryCount = 0, selected = 0, scroll = 0, tunerInstrument = 0;
int pianoOctave=0,pianoEffect=4,pianoVolume=85;
bool musicNowPlaying=false;
int radioSelected=0;
bool radioEditingUrl=false;
String radioEntry;
String radioMessage="SELECT A STATION";
String audioStatusMessage="STOPPED";
String audioNowTitle;
String audioTrackPath, audioTrackExtension, audioTrackArtist, audioTrackAlbum, audioTrackTitle;
static uint8_t audioCoverData[40U*1024U];
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
uint8_t wifiConfigStage=0;
int lastWiFiStatus=-1;
String wifiNetworks[20];
int wifiNetworkRSSI[20];
int wifiNetworkAuth[20];
int wifiNetworkCount=0, wifiSelected=0, wifiScroll=0;
bool wifiScanPending=false;
bool wifiScanOnly=false;
BLEScan* bleScanner=nullptr;
bool bleInitialized=false;
volatile bool bleScanDone=false;
bool bleScanning=false;
String bleNames[8];
int bleRssi[8];
int bleCount=0;
String hackMessage;
uint32_t tunerLastSample=0;
float tunerFrequency=0.0f, tunerCents=0.0f;
int tunerMidi=-1;
int16_t tunerSamples[3072];
float tunerFilteredFrequency=0.0f;
bool tunerHasSignal=false;
uint32_t tunerLastSignalAt=0;
bool usbActive=false;
String usbCommandLine;
bool micStarted=false;
uint32_t micWarmupUntil=0;
int8_t synthBuffer[8192];
const char* effectNames[]={"CLEAN","OVERDRIVE","FUZZ","SQUARE","GUITAR"};
String selectedFile;
volatile bool audioStarted = false;
volatile bool audioPaused = false;
volatile bool audioIsRadio = false;
SemaphoreHandle_t audioMutex = nullptr;
volatile bool audioTaskEnded = false;
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
const char* apps[] = {"MUSIC","SYSTEM","GAMES","HACK"};
constexpr int APP_COUNT=4;
const char* musicMenuItems[]={"SYNTH","TUNER","MUSIC PLAYER","WEB RADIO","EQUALIZER","MIC RECORDER"};
const char* systemMenuItems[]={"FILES","USB TRANSFER","WI-FI","UPDATE FIRMWARE","DEVICE STATUS"};
const char* gamesMenuItems[]={"CONWAY LIFE"};
const char* hackMenuItems[]={"WI-FI RECON","BLE SCANNER","SPAM FX (OFFLINE)","DEVICE / RF INFO"};
const char* hackFxItems[]={"RICKROLL (LOCAL FILE)","NEVER FADE AWAY (LOCAL FILE)","VISUAL FX ONLY"};
int hackFxSelected=0;
bool hackFxPlaying=false;
bool hackVisualFx=false;
uint32_t hackFxLastFrame=0;
constexpr int MUSIC_MENU_COUNT=6, SYSTEM_MENU_COUNT=5, GAMES_MENU_COUNT=1, HACK_MENU_COUNT=4, HACK_FX_COUNT=3;
String placeholderTitle, placeholderMessage;
String updateMessage="CONNECT TO WI-FI";
File micRecordFile;
bool micRecording=false;
uint32_t micRecordBytes=0,micRecordLastDraw=0;
String micRecordPath,micRecordStatus="READY";
int16_t micRecordSamples[3072];
const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
const float guitarHz[] = {82.41f,110.00f,146.83f,196.00f,246.94f,329.63f};
const char* guitarNames[] = {"E2","A2","D3","G3","B3","E4"};
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
void header(const char* title) {
  auto &d=M5Cardputer.Display;
  d.fillRect(0,18,240,117,BG);
  d.setTextSize(1);
  d.setTextColor(UI_PINK,BG); d.setCursor(7,5); d.print("JOHNNY");
  drawBatteryIndicator();
  d.drawFastHLine(7,18,226,UI_PURPLE);
  d.setTextSize(2); d.setTextColor(WHITE,BG); d.setCursor(8,25); d.print(title);
  d.setTextSize(1);
}
void footer(const char* s="") {
  (void)s;
  M5Cardputer.Display.drawFastHLine(7,117,226,0x5140);
}
void drawHome() {
  header("JOHNNY");auto& d=M5Cardputer.Display;
  const int xs[4]={7,124,7,124};const int ys[4]={43,43,79,79};
  for(int i=0;i<APP_COUNT;i++){
    int x=xs[i],y=ys[i];bool on=i==selected;uint16_t fill=on?0x4810:PANEL;
    d.fillRoundRect(x,y,109,28,5,fill);d.drawRoundRect(x,y,109,28,5,on?UI_PINK:UI_PURPLE);
    d.setTextColor(on?WHITE:UI_MUTED,fill);d.setCursor(x+10,y+10);d.print(apps[i]);
  }
  d.setTextColor(sdReady?UI_LAVENDER:UI_PINK,BG);d.fillCircle(13,113,2,sdReady?UI_LAVENDER:UI_PINK);footer("");
}
void drawCategoryMenu(){
  bool music=screen==MUSICMENU,system=screen==SYSTEMMENU,games=screen==GAMESMENU;
  const char* title=music?"MUSIC":(system?"SYSTEM":(games?"GAMES":"HACK"));
  const char** items=music?musicMenuItems:(system?systemMenuItems:(games?gamesMenuItems:hackMenuItems));
  int count=music?MUSIC_MENU_COUNT:(system?SYSTEM_MENU_COUNT:(games?GAMES_MENU_COUNT:HACK_MENU_COUNT));
  header(title);auto& d=M5Cardputer.Display;
  for(int i=0;i<count;i++){
    int y=43+i*12;bool active=i==selected;uint16_t bg=active?PANEL:BG;
    d.fillRoundRect(7,y-1,225,9,2,bg);
    d.setTextColor(active?UI_PINK:UI_MUTED,bg);d.setCursor(12,y);
    d.printf("%c %02d  %s",active?'>':' ',i+1,items[i]);
  }
  footer("");
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
void showUnavailable(const char* title,const char* message){
  screen=ABOUT;header(title);auto& d=M5Cardputer.Display;d.setTextColor(UI_LAVENDER,BG);
  d.setCursor(8,52);d.print(message);footer("ESC BACK");
}
void splash() {
  auto &d=M5Cardputer.Display;
  d.fillScreen(BG);
  d.setTextSize(3);
  d.setTextColor(WHITE,BG);
  d.setCursor(38,48); d.print("JOHNNY");
  d.setTextSize(1);
  delay(500);
}
void pianoScreen() {
  header("POCKET SYNTH");
  auto &d=M5Cardputer.Display;
  d.setTextColor(UI_MUTED,BG); d.setCursor(8,42);
  d.printf("OCT %d  %s  VOL %d%%",4+pianoOctave,effectNames[pianoEffect],pianoVolume);
  const char* labels[]={"DO","RE","MI","FA","SOL","LA","SI"};
  for(int i=0;i<7;i++){
    int x=5+i*33; uint16_t key=(i==1||i==3||i==6)?0xBDBF:0xEFFF;
    d.fillRoundRect(x,57,30,43,3,key);
    d.setTextColor(BG,key); d.setCursor(x+5,74); d.print(labels[i]);
    d.setCursor(x+10,88); d.printf("%c","QWERTYU"[i]);
  }
  d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,108); d.print("- / + VOLUME   [ / ] OCTAVE");
  footer(", . / ; ARROWS   UP/DOWN TIMBRE");
}
void playNote(char c) {
  const char* keys="qwertyu";
  const char* pos=strchr(keys,(char)tolower((unsigned char)c));
  if(!pos) return;
  const int scale[7]={0,2,4,5,7,9,11};
  const int midi=60+pianoOctave*12+scale[(int)(pos-keys)];
  const float hz=440.0f*powf(2.0f,(midi-69)/12.0f);
  if(audioMutex)xSemaphoreTake(audioMutex,portMAX_DELAY);
  M5Cardputer.Speaker.stop();M5Cardputer.Speaker.setVolume(255);audioEqualizer.reset();  const float gain=(float)pianoVolume*1.27f;
  const float guitarDecay=0.30f+0.34f*(82.0f/max(hz,82.0f));
  for(size_t i=0;i<8192;i++) {
    const float phase=6.2831853f*hz*((float)i/22050.0f);
    float v=sinf(phase);
    if(pianoEffect==1) {      v=tanhf(v*3.8f)/tanhf(3.8f);
    } else if(pianoEffect==2) {
      v=tanhf((sinf(phase)+0.55f*sinf(phase*2.0f)+0.28f*sinf(phase*3.0f))*5.0f);
    } else if(pianoEffect==3) {
      v=(v>=0.0f)?0.72f:-0.72f;
    } else if(pianoEffect==4) {
      // Plucked-string patch: a short pick transient and decaying harmonics.
      const float body=sinf(phase)+0.42f*sinf(phase*2.0f)+0.18f*sinf(phase*3.0f)+0.07f*sinf(phase*4.0f);
      const float decay=expf(-(float)i/(22050.0f*guitarDecay));
      const float pick=0.22f*expf(-(float)i/85.0f)*sinf(phase*6.7f);
      v=(body*decay+pick)*0.72f;
    }
    const float envelope=(i<80)?((float)i/80.0f):1.0f;    v*=envelope;
    synthBuffer[i]=(int8_t)constrain((int)lroundf(v*gain),-127,127);
  }
  static int16_t synthEqBuffer[8192];
  for(size_t i=0;i<8192;i++)synthEqBuffer[i]=audioEqualizer.processMono((int32_t)synthBuffer[i]*256,22050);  M5Cardputer.Speaker.playRaw(synthEqBuffer,8192,22050,false,1,0,true);  if(audioMutex)xSemaphoreGive(audioMutex);
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
float detectPitch(const int16_t* samples, size_t count, float* rmsOut=nullptr) {
  if(count<512) return 0;
  double mean=0;
  for(size_t i=0;i<count;i++) mean+=samples[i];
  mean/=count;
  double energy=0;
  for(size_t i=0;i<count;i++) { double v=(double)samples[i]-mean; energy+=v*v; }
  float rms=sqrtf((float)(energy/count));
  if(rmsOut)*rmsOut=rms;
  if(rms<180.0f) return 0;
  const int minLag=13, maxLag=min(300,(int)count/3);
  float previous=0;
  for(int lag=minLag;lag<=maxLag;lag++) {
    double cross=0,aa=0,bb=0;
    for(size_t i=0;i<count-(size_t)lag;i++) {
      double x=(double)samples[i]-mean, y=(double)samples[i+lag]-mean;
      cross+=x*y; aa+=x*x; bb+=y*y;
    }
    float corr=(aa>0 && bb>0)?(float)(cross/sqrt(aa*bb)):0;
    if(lag>minLag && previous>0.55f && previous>=corr) return 16000.0f/(float)(lag-1);
    previous=corr;
  }
  return 0;
}
int nearestMidi(float hz, float* cents) {
  if(hz<=0) return -1;
  float raw=69.0f+12.0f*log2f(hz/440.0f);
  int midi=(int)lroundf(raw);
  if(cents)*cents=(raw-(float)midi)*100.0f;
  return midi;
}
void drawTunerReadout() {
  auto &d=M5Cardputer.Display;
  d.fillRect(7,55,226,61,BG);
  if(!tunerHasSignal) {
    d.setTextColor(UI_LAVENDER,BG); d.setTextSize(2); d.setCursor(8,68); d.print("PLAY A STRING");
    d.setTextSize(1);
    return;
  }
  const float* hzTable=(tunerInstrument==2)?ukeHz:guitarHz;
  const char** nameTable=(tunerInstrument==2)?ukeNames:guitarNames;
  const int count=(tunerInstrument==2)?4:6;
  int nearest=0;
  float cents=1200.0f*log2f(tunerFrequency/hzTable[0]);
  for(int i=1;i<count;i++) {
    float candidate=1200.0f*log2f(tunerFrequency/hzTable[i]);
    if(fabsf(candidate)<fabsf(cents)) { cents=candidate; nearest=i; }
  }
  const uint16_t statusColor=(fabsf(cents)<=5.0f)?UI_LAVENDER:UI_PINK;
  d.fillRoundRect(8,56,224,51,4,PANEL);
  d.setTextColor(UI_MUTED,PANEL); d.setCursor(16,63); d.printf("STRING %s / %.1f Hz",nameTable[nearest],hzTable[nearest]);
  d.setTextColor(WHITE,PANEL); d.setTextSize(2); d.setCursor(16,78); d.printf("%+.0f CENT",cents);
  d.setTextSize(1); d.setTextColor(UI_LAVENDER,PANEL); d.setCursor(156,83); d.printf("%.1f Hz",tunerFrequency);
  d.drawFastHLine(18,113,204,UI_PURPLE);
  d.drawFastVLine(120,109,9,UI_LAVENDER);
  int marker=120+(int)constrain(cents,-50.0f,50.0f)*2;
  d.fillCircle(marker,113,3,statusColor);
}
void tunerScreen() {
  header("TUNER");
  auto &d=M5Cardputer.Display;
  const char* modeNames[]={"GUITAR","VIOLAO","UKULELE"};
  d.setTextColor(UI_PINK,BG); d.setCursor(8,43); d.print(modeNames[tunerInstrument]);
  d.setTextColor(UI_MUTED,BG); d.setCursor(166,43); d.print("<  >");
  drawTunerReadout();
  footer("");
}
void drawNetworkScreen() {
  header("WI-FI");
  auto &d=M5Cardputer.Display;
  if(wifiConfigStage==1) {
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
      String name=wifiNetworks[i]; if(name.length()>22)name=name.substring(0,19)+"...";
      d.printf("%c %s %ddBm",active?'>':' ',name.c_str(),wifiNetworkRSSI[i]);
      if(wifiNetworkAuth[i]==WIFI_AUTH_OPEN) { d.setTextColor(UI_LAVENDER,bg); d.setCursor(211,y); d.print("O"); }
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
void onBleScanComplete(BLEScanResults results){(void)results;bleScanDone=true;}
void startBleScan(){
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
void drawBleScreen(){
  header("BLE SCANNER");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,42);d.print(bleScanning?"PASSIVE SCAN IN PROGRESS":hackMessage.substring(0,28));
  for(int i=0;i<min(5,bleCount);i++){int y=57+i*11;d.setTextColor(WHITE,BG);d.setCursor(9,y);d.print(bleNames[i].substring(0,20));d.setTextColor(UI_MUTED,BG);d.setCursor(194,y);d.printf("%d",bleRssi[i]);}
  footer("R RESCAN  ESC BACK");
}
void drawHackInfo(){
  header("WIRELESS LAB");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,46);d.print("PASSIVE TOOLS ONLY");
  d.setTextColor(UI_MUTED,BG);d.setCursor(8,65);d.print("NO PACKET FLOODING");
  d.setCursor(8,80);d.print("NO DECEPTIVE FREE WI-FI");
  d.setCursor(8,98);d.print("USE ON YOUR OWN NETWORK");footer("ESC BACK");
}
void drawHackFxScreen(){
  header("SPAM FX / OFFLINE");auto& d=M5Cardputer.Display;
  if(hackFxPlaying){
    d.setTextColor(UI_PINK,BG);d.setCursor(8,43);d.print("LOCAL AUDIO DEMO");
    String title=audioTrackTitle.length()?audioTrackTitle:audioNowTitle;if(title.length()>30)title=title.substring(0,27)+"...";
    d.setTextColor(WHITE,BG);d.setCursor(8,61);d.print(title);
    d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,77);d.print(audioTrackArtist.length()?audioTrackArtist:"LOCAL FILE");
    d.drawFastHLine(8,96,224,0x5140);d.drawFastHLine(8,96,(int)audioProgressPercent()*224/100,UI_PINK);
    d.setCursor(8,108);d.print(audioStarted?(audioPaused?"PAUSED":"PLAYING"):audioStatusMessage.substring(0,24));
    footer("ENTER PAUSE  P STOP  ESC BACK");return;
  }
  if(hackVisualFx){d.setTextColor(UI_PINK,BG);d.setCursor(8,48);d.print("OFFLINE VISUAL DEMO");d.setTextColor(UI_LAVENDER,BG);d.setCursor(8,68);d.print("NO WI-FI / BLE PACKETS");d.setCursor(8,85);d.print("ARE TRANSMITTED");footer("ESC BACK");return;}
  d.setTextColor(UI_MUTED,BG);d.setCursor(8,42);d.print("LOCAL MEDIA / NO RF TX");
  for(int i=0;i<HACK_FX_COUNT;i++){int y=59+i*15;bool active=i==hackFxSelected;uint16_t bg=active?PANEL:BG;d.fillRoundRect(7,y-1,225,12,2,bg);d.setTextColor(active?UI_PINK:UI_MUTED,bg);d.setCursor(12,y);d.print(active?"> ":"  ");d.print(hackFxItems[i]);}
  footer("ENTER RUN LOCAL DEMO  ESC BACK");
}

void startNetworkSetup() {
  WiFi.mode(WIFI_STA); WiFi.disconnect(false,false);
  wifiNetworkCount=0; wifiSelected=0; wifiScroll=0; wifiScanPending=true;
  wifiConfigStage=1; WiFi.scanDelete(); WiFi.scanNetworks(true,false);
  drawNetworkScreen();
}
void startWifiSurvey() {
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
void drawHackFxScreen();void drawHackInfo();void runHackAudio(const String& keyword,const String& label);
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
  return name.endsWith(".mp3") || name.endsWith(".wav") || name.endsWith(".flac") ||
         name.endsWith(".aac") || name.endsWith(".ogg") || name.endsWith(".opus") || name.endsWith(".oga");
}
void scanDirectory(bool musicMode=false) {
  entryCount=0; selected=0; scroll=0;
  if(!sdReady)return;
  File dir=SD.open(currentPath);
  if(!dir || !dir.isDirectory()) { if(dir)dir.close(); return; }
  File f=dir.openNextFile();
  while(f && entryCount<48) {
    String name=String(f.name());
    int slash=name.lastIndexOf('/');
    if(slash>=0) name=name.substring(slash+1);
    if(name.length() && name!="System Volume Information" &&
       (!musicMode || f.isDirectory() || isPlayableAudio(name))) {
      entries[entryCount]=name;
      entryIsDir[entryCount]=f.isDirectory();
      entryCount++;
    }
    f.close(); f=dir.openNextFile();
  }
  dir.close();
  for(int i=0;i<entryCount-1;i++) for(int j=i+1;j<entryCount;j++)    if((!entryIsDir[i] && entryIsDir[j]) || (entryIsDir[i]==entryIsDir[j] && entries[j].compareTo(entries[i])<0)) {
      String t=entries[i]; entries[i]=entries[j]; entries[j]=t;
      bool b=entryIsDir[i]; entryIsDir[i]=entryIsDir[j]; entryIsDir[j]=b;
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
  footer(",./; MOVE  ENTER OPEN  ESC BACK");
}

void drawMusicInfo(const String &name);
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
}
void stopAudio() {
  if(audioMutex) xSemaphoreTake(audioMutex,portMAX_DELAY);
  stopAudioLocked();  audioTaskEnded=false;
  if(audioMutex) xSemaphoreGive(audioMutex);
  // Let the SD writer finish the WAV header before another app accesses the card.
  uint32_t started=millis();
  while(radioRecordState==3 && (uint32_t)(millis()-started)<5000UL) delay(2);
}
AudioGenerator* createAudioDecoder(const String& extension) {
  if(extension=="mp3") return new AudioGeneratorMP3();
  if(extension=="wav") return new AudioGeneratorWAV();
  if(extension=="flac") return new AudioGeneratorFLAC();
  if(extension=="aac") return new AudioGeneratorAAC();
  if(extension=="ogg" || extension=="opus" || extension=="oga") return new AudioGeneratorOpus();
  return nullptr;
}
bool startAudioSource(AudioFileSource* source, const String& title, const String& extension, bool radio) {
  if(!source) { audioStatusMessage="SOURCE ERROR"; return false; }
  if(!audioTaskReady || !audioMutex) {
    source->close(); delete source;
    audioStatusMessage="AUDIO TASK UNAVAILABLE";
    return false;
  }  xSemaphoreTake(audioMutex,portMAX_DELAY);
  stopAudioLocked();
  audioRawSource=source;
  audioInputBuffer=new AudioFileSourceBuffer(audioRawSource,8192);
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
    audioStatusMessage=(extension=="ogg" || extension=="opus" || extension=="oga")?
      "OGG MUST BE OPUS":"DECODE / STREAM ERROR";
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
void loadTrackMetadata(const String& path){
  audioTrackPath=path;audioTrackExtension=audioExtension(path);audioTrackArtist="";audioTrackAlbum="";audioTrackTitle=path.substring(path.lastIndexOf('/')+1);int dot=audioTrackTitle.lastIndexOf('.');if(dot>0)audioTrackTitle.remove(dot);audioCoverSize=0;audioCoverPng=false;
  File file=SD.open(path,FILE_READ);if(file){uint8_t h[10]={0};if(file.read(h,sizeof(h))==sizeof(h)&&memcmp(h,"ID3",3)==0&&h[3]>=3&&h[3]<=4){uint32_t tagSize=id3Synchsafe32(h+6),tagEnd=min((uint32_t)file.size(),10U+tagSize);uint8_t fh[10];while((uint32_t)file.position()+10U<=tagEnd){if(file.read(fh,sizeof(fh))!=sizeof(fh)||fh[0]==0)break;char id[5]={(char)fh[0],(char)fh[1],(char)fh[2],(char)fh[3],0};uint32_t frameSize=h[3]==4?id3Synchsafe32(fh+4):id3BigEndian32(fh+4);if(frameSize==0)continue;uint32_t frameEnd=(uint32_t)file.position()+frameSize;if(frameEnd>tagEnd||frameEnd>(uint32_t)file.size())break;
      if((strcmp(id,"TIT2")==0||strcmp(id,"TPE1")==0||strcmp(id,"TALB")==0)&&frameSize<=512){uint8_t txt[512];if(file.read(txt,frameSize)==frameSize){String v=decodeId3Text(txt,frameSize);if(strcmp(id,"TIT2")==0&&v.length())audioTrackTitle=v;else if(strcmp(id,"TPE1")==0)audioTrackArtist=v;else if(strcmp(id,"TALB")==0)audioTrackAlbum=v;}}
      else if(strcmp(id,"APIC")==0&&frameSize<=sizeof(audioCoverData)){if(file.read(audioCoverData,frameSize)==frameSize)extractApicImage(frameSize);}
      file.seek(frameEnd);
    }}file.close();}
  if(audioCoverSize==0)loadSidecarCover(path);
}

bool startLocalAudio(const String& path) {
  if(!sdReady) { audioStatusMessage="NO SD CARD"; return false; }
  stopAudio();loadTrackMetadata(path);
  const String ext=audioExtension(path);
  if(ext!="mp3" && ext!="wav" && ext!="flac" && ext!="aac" && ext!="ogg" && ext!="opus" && ext!="oga") {
    audioStatusMessage="UNSUPPORTED FORMAT";
    return false;
  }
  if((ext=="ogg" || ext=="opus" || ext=="oga") && !isOggOpusFile(path)) {
    audioStatusMessage="OGG VORBIS NOT SUPPORTED";
    return false;
  }
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

bool findAudioKeywordRecursive(const String& directory,const String& keyword,String& found,int depth){
  if(depth<0)return false;File dir=SD.open(directory);if(!dir||!dir.isDirectory()){if(dir)dir.close();return false;}File entry=dir.openNextFile();
  while(entry){String raw=String(entry.name());bool isDir=entry.isDirectory();entry.close();String path=raw.startsWith("/")?raw:(directory=="/"?"/"+raw:directory+"/"+raw);
    if(isDir){if(path!="/System Volume Information"&&path!="/JOHNNY"&&findAudioKeywordRecursive(path,keyword,found,depth-1)){dir.close();return true;}}
    else{String low=raw;low.toLowerCase();if(isPlayableAudio(low)&&low.indexOf(keyword)>=0){found=path;dir.close();return true;}}
    entry=dir.openNextFile();
  }dir.close();return false;
}
void runHackAudio(const String& keyword,const String& label){
  stopAudio();hackFxPlaying=false;hackVisualFx=false;String path;
  if(!sdReady||!findAudioKeywordRecursive("/",keyword,path,3)){hackMessage=label+" FILE NOT FOUND ON MICROSD";drawHackFxScreen();return;}
  bool ok=startLocalAudio(path);hackFxPlaying=ok;if(!ok)hackMessage=audioStatusMessage;else hackMessage=label+" PLAYING";drawHackFxScreen();
}

bool startRadioStream(const String& url, const String& title) {
  stopAudio();
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
  if(xSemaphoreTake(audioMutex,pdMS_TO_TICKS(250))!=pdTRUE){audioStatusMessage="AUDIO BUSY";return false;}
  if(!audioRawSource||!audioInputBuffer||!audioDecoder){xSemaphoreGive(audioMutex);return false;}
  uint32_t size=audioRawSource->getSize(),pos=audioRawSource->getPos(),buffered=audioInputBuffer->getFillLevel();pos=pos>buffered?pos-buffered:0;
  if(size<4096){xSemaphoreGive(audioMutex);return false;}
  int64_t target=(int64_t)pos+(int64_t)direction*(int64_t)max((uint32_t)4096,size/20U);if(target<0)target=0;if(target>(int64_t)size-1)target=(int64_t)size-1;
  bool paused=audioPaused;audioDecoder->stop();delete audioDecoder;audioDecoder=nullptr;delete audioInputBuffer;audioInputBuffer=nullptr;
  if(!audioRawSource->seek((int32_t)target,SEEK_SET))audioRawSource->seek(0,SEEK_SET);
  audioInputBuffer=new AudioFileSourceBuffer(audioRawSource,8192);audioDecoder=createAudioDecoder(audioTrackExtension);
  bool ok=audioInputBuffer&&audioDecoder&&audioDecoder->begin(audioInputBuffer,audioOutput);
  if(ok){audioStarted=true;audioPaused=paused;audioTaskEnded=false;audioStatusMessage=paused?"PAUSED":"PLAYING";audioEqualizer.reset();}
  else{if(audioDecoder){delete audioDecoder;audioDecoder=nullptr;}if(audioInputBuffer){delete audioInputBuffer;audioInputBuffer=nullptr;}audioStarted=false;audioPaused=true;audioStatusMessage="SEEK FAILED";}
  xSemaphoreGive(audioMutex);return ok;
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
  else if(screen==MUSIC&&musicNowPlaying&&entryCount>0){if(audioRepeatMode==2)playSelectedTrack();else if(audioShuffle)moveMusicTrack(1);else if(selected<entryCount-1)moveMusicTrack(1);else if(audioRepeatMode==1){selected=0;while(selected<entryCount&&entryIsDir[selected])selected++;if(selected<entryCount)playSelectedTrack();}}
  if(!screenSaverActive){if(screen==MUSIC&&musicNowPlaying)drawMusicInfo(audioTrackTitle.length()?audioTrackTitle:audioNowTitle);else if(screen==RADIO)drawRadioScreen();}
}
void audioTask(void* parameter) {
  (void)parameter;
  audioTaskReady=true;
  for(;;) {
    if(audioMutex && xSemaphoreTake(audioMutex,pdMS_TO_TICKS(50))==pdTRUE) {
      if(audioStarted && !audioPaused && audioDecoder) {
        if(!audioDecoder->loop()) {
          audioTaskEndedWasRadio=audioIsRadio;
          audioTaskEnded=true;
          audioStarted=false;
          audioPaused=true;
        }
      }
      xSemaphoreGive(audioMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
void toggleAudioPause() {
  if(audioMutex) xSemaphoreTake(audioMutex,portMAX_DELAY);
  audioPaused=!audioPaused;
  if(audioMutex) xSemaphoreGive(audioMutex);
}
void setAudioVolume(int value) {
  if(audioMutex) xSemaphoreTake(audioMutex,portMAX_DELAY);
  audioVolume=constrain(value,0,21);
  if(audioOutput) audioOutput->SetGain((float)audioVolume/21.0f);
  if(audioMutex) xSemaphoreGive(audioMutex);
}
void drawMusicInfo(const String &name){
  header("JOHNNY MUSIC");auto& d=M5Cardputer.Display;
  d.setTextColor(UI_MUTED,BG);d.setCursor(8,39);d.print("NOW PLAYING");d.fillRoundRect(8,49,62,62,5,PANEL);
  bool cover=false;if(audioCoverSize>0)cover=audioCoverPng?d.drawPng(audioCoverData,audioCoverSize,9,50,60,60):d.drawJpg(audioCoverData,audioCoverSize,9,50,60,60);
  if(!cover){d.drawCircle(39,80,20,UI_PURPLE);d.drawCircle(39,80,13,UI_PINK);d.drawCircle(39,80,5,UI_LAVENDER);}
  d.setTextColor(WHITE,BG);String title=audioTrackTitle.length()?audioTrackTitle:name;if(title.length()>23)title=title.substring(0,20)+"...";d.setCursor(78,51);d.print(title);
  d.setTextColor(UI_LAVENDER,BG);String artist=audioTrackArtist.length()?audioTrackArtist:"UNKNOWN ARTIST";if(artist.length()>24)artist=artist.substring(0,21)+"...";d.setCursor(78,66);d.print(artist);
  String album=audioTrackAlbum.length()?audioTrackAlbum:"LOCAL LIBRARY";if(album.length()>24)album=album.substring(0,21)+"...";d.setTextColor(UI_MUTED,BG);d.setCursor(78,80);d.print(album);
  d.drawFastHLine(78,96,151,0x5140);uint8_t pct=audioProgressPercent();d.drawFastHLine(78,96,(int)pct*151/100,UI_PINK);d.fillCircle(78+(int)pct*151/100,96,3,UI_LAVENDER);
  d.setCursor(78,103);d.printf("%3u%%",pct);d.setCursor(125,103);d.printf("%s REP:%s",audioShuffle?"SHUF":"ORDER",audioRepeatMode==0?"OFF":(audioRepeatMode==1?"ALL":"ONE"));
  d.setTextColor(audioStarted?(audioPaused?UI_PINK:UI_LAVENDER):UI_MUTED,BG);d.setCursor(8,115);d.printf("%s VOL %02d",audioStarted?(audioPaused?"PAUSED":"PLAYING"):audioStatusMessage.c_str(),audioVolume);
  footer("< > TRACK  UP/DN SEEK  ENTER PAUSE");
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
  String path=currentPath;
  if(path!="/") path+="/";
  path+=entries[selected];
  audioStatusMessage="LOADING";
  const bool ok=startLocalAudio(path);
  musicNowPlaying=true;
  drawMusicInfo(entries[selected]);
  if(!ok) {
    M5Cardputer.Display.setTextColor(UI_PINK,BG);
    M5Cardputer.Display.setCursor(8,88);
    M5Cardputer.Display.print(audioStatusMessage.substring(0,28));
  }
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
  String full=currentPath;
  if(full!="/") full+="/";  full+=entries[selected];
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
  if(screen==HOME){screen=selected==0?MUSICMENU:(selected==1?SYSTEMMENU:(selected==2?GAMESMENU:HACKMENU));selected=0;drawCategoryMenu();return;}
  int item=selected;Screen menu=screen;
  if(menu==HACKMENU){if(item==0){screen=NETWORK;startWifiSurvey();return;}if(item==1){screen=BLESCAN;startBleScan();drawBleScreen();return;}if(item==2){screen=HACKFX;hackFxSelected=0;hackFxPlaying=false;hackVisualFx=false;drawHackFxScreen();return;}screen=HACKINFO;drawHackInfo();return;}
  if(menu!=MUSICMENU&&menu!=SYSTEMMENU&&menu!=GAMESMENU)return;screen=HOME;
  if(menu==MUSICMENU){if(item==5){screen=MICREC;micRecording=false;micRecordStatus="READY";drawMicRecorder();return;}const int map[]={0,1,2,3,7};selected=map[item];}
  else if(menu==SYSTEMMENU){if(item==3){screen=UPDATEINFO;updateMessage="PRESS ENTER TO INSTALL LATEST";drawUpdateInfo();return;}if(item==4){selected=9;openLegacyApp();return;}const int map[]={4,6,5};selected=map[item];}
  else selected=8;
  openLegacyApp();
}
void openLegacyApp() {
  switch(selected) {
    case 0: stopMicCapture(); stopAudio(); screen=PIANO; pianoScreen(); break;
    case 1:
      stopAudio(); stopMicCapture(); screen=TUNER; tunerInstrument=1;
      tunerMidi=-1; tunerFrequency=0; tunerFilteredFrequency=0; tunerHasSignal=false;
      tunerLastSignalAt=0; tunerLastSample=0; tunerScreen(); break;
    case 2:
      stopAudio();
      screen=MUSIC;musicNowPlaying=false;currentPath="/";scanDirectory(true);drawFiles(true);break;
    case 3:
      if(!audioIsRadio && audioStarted) stopAudio();
      screen=RADIO; radioEditingUrl=false; radioSelected=0; drawRadioScreen(); break;
    case 4:
      stopAudio();
      {uint32_t started=millis();while(radioRecordState==3&&millis()-started<5000UL)delay(5);}
      screen=FILES;currentPath="/";scanDirectory(false);drawFiles();break;
    case 5: screen=NETWORK; wifiConfigStage=0; lastWiFiStatus=-1; drawNetworkScreen(); break;
    case 6: screen=USBMODE;drawUsbScreen(beginUsbMode());break;
    case 7: eqReturnScreen=MUSICMENU;screen=EQSCREEN;selected=4;drawEqScreen();break;
    case 8: screen=LIFE;if(!loadLife())seedLife();drawLife();break;
    case 9: screen=ABOUT;header("DEVICE STATUS");M5Cardputer.Display.setTextColor(WHITE,BG);M5Cardputer.Display.setCursor(8,49);M5Cardputer.Display.print("JOHNNY / CARDPUTER ADV");M5Cardputer.Display.setTextColor(UI_LAVENDER,BG);M5Cardputer.Display.setCursor(8,67);M5Cardputer.Display.printf("SD: %s",sdReady?"READY":"NOT FOUND");M5Cardputer.Display.setCursor(8,81);M5Cardputer.Display.printf("WI-FI: %s",WiFi.status()==WL_CONNECTED?"CONNECTED":"OFFLINE");M5Cardputer.Display.setCursor(8,95);M5Cardputer.Display.printf("BLE: %s",bleInitialized?"READY":"IDLE");M5Cardputer.Display.setCursor(8,109);M5Cardputer.Display.printf("BATTERY: %d%%",batteryPercent());footer("ESC BACK");break;
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
  SD.mkdir("/RECORDINGS");String candidate;
  for(int i=1;i<=9999;i++){char name[32];snprintf(name,sizeof(name),"/RECORDINGS/MIC%04d.WAV",i);candidate=String(name);if(!SD.exists(candidate))break;}
  if(candidate.length()==0||SD.exists(candidate)){micRecordStatus="NO FREE FILENAME";drawMicRecorder();return;}
  micRecordFile=SD.open(candidate,FILE_WRITE);if(!micRecordFile){micRecordStatus="FILE OPEN FAILED";drawMicRecorder();return;}
  uint8_t blank[44]{};micRecordFile.write(blank,sizeof(blank));micRecordPath=candidate;micRecordBytes=0;micRecording=true;micRecordStatus="RECORDING PCM 16 KHZ";micRecordLastDraw=0;drawMicRecorder();
}
void stopMicRecording(){
  if(!micRecording)return;
  micRecording=false;
  if(micRecordFile){writeWavHeader(micRecordFile,micRecordBytes,16000);micRecordFile.flush();micRecordFile.close();}
  stopMicCapture();micRecordStatus="SAVED TO MICROSD";drawMicRecorder();
}
void serviceMicRecorder(){
  if(!micRecording)return;
  if(!captureMic(micRecordSamples,3072,420)){if(millis()-micRecordLastDraw>500){micRecordLastDraw=millis();if(!screenSaverActive)drawMicRecorder();}return;}
  size_t bytes=sizeof(micRecordSamples);
  if(!micRecordFile||micRecordFile.write((const uint8_t*)micRecordSamples,bytes)!=bytes){micRecordStatus="SD WRITE ERROR";stopMicRecording();return;}
  micRecordBytes+=(uint32_t)bytes;
  if(millis()-micRecordLastDraw>500){micRecordLastDraw=millis();if(!screenSaverActive)drawMicRecorder();}
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
void setup() {
  auto cfg=M5.config();
  M5Cardputer.begin(cfg,true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(255);
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
  radioEntry=preferences.getString("radio_url","");
  eqPreset=preferences.getInt("eqpreset",1);if(eqPreset<0||eqPreset>3)eqPreset=-1;
  for(int i=0;i<(int)JOHNNY_EQ_BANDS;i++){
    float fallback=eqPreset>=0?eqPresets[eqPreset][i]:eqPresets[1][i];
    audioEqGainDb[i]=preferences.getFloat((String("eq")+String(i)).c_str(),fallback);
  }
  audioEqualizer.setAll(audioEqGainDb);
  if(savedSsid.length()) {
    WiFi.mode(WIFI_STA); WiFi.setSleep(false);
    WiFi.begin(savedSsid.c_str(),savedPassword.c_str());
  }
  auto micCfg=M5Cardputer.Mic.config();
  micCfg.sample_rate=16000;
  M5Cardputer.Mic.config(micCfg);
  refreshBatteryCache(true);
  if(audioMutex)xTaskCreatePinnedToCore(audioTask,"JOHNNY_AUDIO",10240,nullptr,3,nullptr,1);
  radioRecordQueue=xQueueCreate(8,sizeof(RadioRecordBlock));
  if(radioRecordQueue)xTaskCreatePinnedToCore(radioRecorderTask,"JOHNNY_REC",4096,nullptr,2,nullptr,0);
  splash();selected=0;drawHome();
}
void loop() {
  M5Cardputer.update();
  if(millis()-batteryLastSample>=30000UL) {
    batteryLastDraw=millis();
    refreshBatteryCache(true);
    if(screen!=USBMODE && !screenSaverActive) { drawBatteryIndicator(); if(screen==ABOUT) { header("SYSTEM"); M5Cardputer.Display.setTextColor(WHITE,BG); M5Cardputer.Display.setCursor(8,53); M5Cardputer.Display.print("JOHNNY / CARDPUTER-ADV"); M5Cardputer.Display.setCursor(8,69); M5Cardputer.Display.print("ESP32-S3  |  8 MB FLASH"); M5Cardputer.Display.setCursor(8,85); M5Cardputer.Display.printf("SD: %s",sdReady?"MOUNTED":"NOT FOUND"); M5Cardputer.Display.setCursor(8,91); M5Cardputer.Display.printf("WI-FI: %s",WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE"); int battery=batteryPercent(); int16_t mv=batteryMillivolts(); M5Cardputer.Display.setCursor(8,103); if(battery>=0 && mv>0) M5Cardputer.Display.printf("BATTERY: %d%%  %.2fV",constrain(battery,0,100),mv/1000.0f); else if(battery>=0) M5Cardputer.Display.printf("BATTERY: %d%%  VOLT N/A",constrain(battery,0,100)); else M5Cardputer.Display.print("BATTERY: SENSOR UNAVAILABLE"); M5Cardputer.Display.setCursor(8,115); M5Cardputer.Display.setTextColor(batteryCharging()?UI_PINK:UI_MUTED,BG); M5Cardputer.Display.print(batteryCharging()?"CHARGING  [USB POWER]":(battery>=0 && battery<=15?"LOW BATTERY":"ON BATTERY")); footer("ESC BACK"); } }
  }
  serviceAudio();
  if(screen==MICREC&&micRecording)serviceMicRecorder();
  if(!screenSaverActive&&screen==LIFE&&lifeRunning&&millis()-lifeLastStep>=lifeIntervalMs){stepLife();drawLife();}
  static uint8_t lastRecorderDisplayState=255;
  if(screen==RADIO&&!screenSaverActive&&lastRecorderDisplayState!=radioRecordState){lastRecorderDisplayState=radioRecordState;drawRadioScreen();}
  if(!screenSaverActive && screen==NETWORK && wifiConfigStage==0) {
    int status=(int)WiFi.status();
    if(status!=lastWiFiStatus) { lastWiFiStatus=status; drawNetworkScreen(); }
  }
  if(!screenSaverActive && screen==NETWORK && wifiConfigStage==1 && wifiScanPending) {
    int result=WiFi.scanComplete();
    if(result>=0) {
      wifiNetworkCount=min(result,20);
      for(int i=0;i<wifiNetworkCount;i++) { wifiNetworks[i]=WiFi.SSID(i); wifiNetworkRSSI[i]=WiFi.RSSI(i); wifiNetworkAuth[i]=(int)WiFi.encryptionType(i); }
      wifiScanPending=false; WiFi.scanDelete();
      if(wifiSelected>=wifiNetworkCount) wifiSelected=max(0,wifiNetworkCount-1);
      drawNetworkScreen();
    } else if(result==WIFI_SCAN_FAILED) { wifiScanPending=false; drawNetworkScreen(); }
  }
  if(screen==TUNER && !screenSaverActive && millis()-tunerLastSample>420) {
    tunerLastSample=millis();
    if(captureMic(tunerSamples,3072,420)) {
      float freq=detectPitch(tunerSamples,3072);
      if(freq>0 && freq<1400) {
        if(tunerFilteredFrequency<=0.0f || fabsf(freq-tunerFilteredFrequency)>tunerFilteredFrequency*0.22f) tunerFilteredFrequency=freq;
        else tunerFilteredFrequency=0.62f*tunerFilteredFrequency+0.38f*freq;
        tunerFrequency=tunerFilteredFrequency; tunerHasSignal=true; tunerLastSignalAt=millis();
        tunerMidi=nearestMidi(tunerFrequency,&tunerCents);
      }
    }
    if(tunerHasSignal && millis()-tunerLastSignalAt>5000UL) {tunerHasSignal=false;tunerFilteredFrequency=0;tunerFrequency=0;tunerMidi=-1;}
    drawTunerReadout();
  }
  
  if(Serial.available()>0) serialTransferTask();
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
    return;
  }

  bool back=(c==0x1B || c=='`' || c=='~');
  if(back) {
    if(screen==MICREC){stopMicRecording();screen=HOME;drawHome();}
    else if(screen==UPDATEINFO){screen=SYSTEMMENU;selected=4;drawCategoryMenu();}
    else if(screen==MUSICMENU||screen==SYSTEMMENU||screen==GAMESMENU||screen==HACKMENU){screen=HOME;selected=0;drawHome();}
    else if(screen==BLESCAN){if(bleScanning&&bleScanner)bleScanner->stop();bleScanning=false;bleScanDone=false;screen=HACKMENU;selected=1;drawCategoryMenu();}
    else if(screen==HACKINFO){screen=HACKMENU;selected=3;drawCategoryMenu();}
    else if(screen==HACKFX){if(hackFxPlaying)stopAudio();hackFxPlaying=false;hackVisualFx=false;screen=HACKMENU;selected=2;drawCategoryMenu();}
    else if(screen==NETWORK&&wifiScanOnly){wifiScanOnly=false;wifiConfigStage=0;screen=HACKMENU;selected=0;drawCategoryMenu();}
    else if(screen==EQSCREEN){
      screen=eqReturnScreen;
      if(screen==HOME)drawHome();
      else if(screen==MUSICMENU)drawCategoryMenu();
      else if(screen==PIANO)pianoScreen();
      else if(screen==MUSIC)musicNowPlaying?drawMusicInfo(audioNowTitle):drawFiles(true);
      else if(screen==RADIO)drawRadioScreen();
    }else if(screen==LIFE){saveLife();screen=HOME;drawHome();}
    else if(screen==FILES || screen==MUSIC) {
      bool music=screen==MUSIC;
      if(music && musicNowPlaying) {musicNowPlaying=false;drawFiles(true);}
      else if(currentPath!="/") {currentPath=parentPath(currentPath);scanDirectory(music);drawFiles(music);}
      else {screen=HOME;drawHome();}
    } else if(screen==RADIO) {
      if(radioEditingUrl) {radioEditingUrl=false;drawRadioScreen();}
      else {screen=HOME;drawHome();}
    } else if(screen==USBMODE) {
if(usbActive) endUsbMode();
      screen=HOME;drawHome();
    } else {
      if(screen==TUNER) stopMicCapture();
      if(screen==NETWORK && wifiConfigStage==2) { wifiConfigStage=1; wifiEntry=""; drawNetworkScreen(); }
      else { wifiConfigStage=0; screen=HOME;drawHome(); }
    }
    delay(8); return;
  }

  if(screen==MICREC) {
    if(k.enter||k.space){if(micRecording)stopMicRecording();else startMicRecording();}
  } else if(screen==UPDATEINFO) {
    if(k.enter) runFirmwareUpdate();
  } else if(screen==MUSICMENU||screen==SYSTEMMENU||screen==GAMESMENU||screen==HACKMENU) {
    int count=screen==MUSICMENU?MUSIC_MENU_COUNT:(screen==SYSTEMMENU?SYSTEM_MENU_COUNT:(screen==GAMESMENU?GAMES_MENU_COUNT:HACK_MENU_COUNT));
    if(k.enter){openApp();delay(8);return;}
    if(c==';')selected=max(0,selected-1);
    else if(c=='.')selected=min(count-1,selected+1);
    drawCategoryMenu();
  } else if(screen==HOME) {
    if(k.enter) { openApp(); delay(8); return; }
    if(c==',')selected=(selected+APP_COUNT-1)%APP_COUNT;
    else if(c=='/')selected=(selected+1)%APP_COUNT;
    drawHome();
  } else if(screen==PIANO) {
    if(c==','){pianoOctave=max(-2,pianoOctave-1);pianoScreen();}
    else if(c=='/'){pianoOctave=min(2,pianoOctave+1);pianoScreen();}
    else if(c==';'){pianoEffect=(pianoEffect+4)%5;pianoScreen();}
    else if(c=='.'){pianoEffect=(pianoEffect+1)%5;pianoScreen();}
    else if(c=='-'){pianoVolume=max(0,pianoVolume-10);pianoScreen();}
    else if(c=='='||c=='+'){pianoVolume=min(100,pianoVolume+10);pianoScreen();}
    else if(c=='['){pianoOctave=max(-2,pianoOctave-1);pianoScreen();}
    else if(c==']'){pianoOctave=min(2,pianoOctave+1);pianoScreen();}
    else if(c)playNote(c);
  } else if(screen==TUNER) {
    if(c==','||c=='/') {tunerInstrument=(tunerInstrument+1)%3;tunerScreen();}
  } else if(screen==MUSIC) {
    if(musicNowPlaying) {
      if(c==',')moveMusicTrack(-1);
      else if(c=='/') moveMusicTrack(1);
      else if(k.enter||k.space||c==' ') {toggleAudioPause();drawMusicInfo(audioNowTitle);}
      else if(c=='p'||c=='P') {stopAudio();audioStatusMessage="STOPPED";drawMusicInfo(audioNowTitle);}
      else if(c=='-'||c=='_') {setAudioVolume(audioVolume-1);drawMusicInfo(audioNowTitle);}
      else if(c=='='||c=='+') {setAudioVolume(audioVolume+1);drawMusicInfo(audioNowTitle);}
      else if(c==';') {seekAudioStep(-1);drawMusicInfo(audioTrackTitle.length()?audioTrackTitle:audioNowTitle);}
      else if(c=='.') {seekAudioStep(1);drawMusicInfo(audioTrackTitle.length()?audioTrackTitle:audioNowTitle);}
      else if(c=='s'||c=='S'){audioShuffle=!audioShuffle;drawMusicInfo(audioNowTitle);}
      else if(c=='r'||c=='R'){audioRepeatMode=(audioRepeatMode+1)%3;drawMusicInfo(audioNowTitle);}
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
  } else if(screen==HACKINFO) {
    // ESC returns to the HACK menu.
  } else if(screen==HACKFX) {
    if(hackFxPlaying){if(k.enter||k.space){toggleAudioPause();drawHackFxScreen();}else if(c=='p'||c=='P'){stopAudio();hackFxPlaying=false;drawHackFxScreen();}}
    else if(!hackVisualFx){if(c==';')hackFxSelected=max(0,hackFxSelected-1);else if(c=='.')hackFxSelected=min(HACK_FX_COUNT-1,hackFxSelected+1);else if(k.enter){if(hackFxSelected==0)runHackAudio("rick","RICKROLL");else if(hackFxSelected==1)runHackAudio("never fade away","NEVER FADE AWAY");else{hackVisualFx=true;hackFxLastFrame=0;drawHackFxScreen();}}if(!hackVisualFx)drawHackFxScreen();}
  } else if(screen==NETWORK) {
    if(wifiConfigStage==0) {
      if(k.enter) startNetworkSetup();
      else if(c=='.') {WiFi.disconnect(false,false);drawNetworkScreen();}
    } else if(wifiConfigStage==1) {
      if(c=='r'||c=='R') {if(wifiScanOnly)startWifiSurvey();else startNetworkSetup();}
      else if(c==';') {if(wifiSelected>0)wifiSelected--;if(wifiSelected<wifiScroll)wifiScroll=wifiSelected;drawNetworkScreen();}
      else if(c=='.') {if(wifiSelected<wifiNetworkCount-1)wifiSelected++;if(wifiSelected>=wifiScroll+5)wifiScroll=wifiSelected-4;drawNetworkScreen();}
      else if(k.enter && !wifiScanPending) {if(wifiScanOnly){hackMessage=wifiNetworks[wifiSelected]+" "+String(wifiNetworkRSSI[wifiSelected])+" dBm";drawNetworkScreen();}else chooseNetwork(wifiSelected);}
    } else if(wifiConfigStage==2) {
      if(k.enter) saveNetworkSetup();
      else if(c && isprint((unsigned char)c) && wifiEntry.length()<63) {wifiEntry+=c;drawNetworkScreen();}
    }
  } else if(screen==USBMODE) {
    // USB transfer is serviced through the serial protocol above.
  }
}
