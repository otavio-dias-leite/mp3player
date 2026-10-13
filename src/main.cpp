#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ctype.h>
#include "sdkconfig.h"
#ifndef ARDUINO_USB_MODE
#define ARDUINO_USB_MODE 1
#endif
#if defined(CONFIG_TINYUSB_MSC_ENABLED) && CONFIG_TINYUSB_MSC_ENABLED && (ARDUINO_USB_MODE == 0)
  #include "USB.h"
  #include "USBMSC.h"
  #include <SdFat.h>
  #define JOHNNY_USB_MSC 1
#else
  #define JOHNNY_USB_MSC 0
#endif

namespace {
constexpr int SD_CS = 12, SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14;
constexpr uint16_t BG = 0x1243, PANEL = 0x24C7, UI_PURPLE = 0xA2DF, UI_PINK = 0xF19F, UI_CYAN = 0x6EFF, UI_MUTED = 0xB5B4;
enum Screen : uint8_t { HOME, PIANO, TUNER, FILES, MUSIC, GALLERY, VOICE, NETWORK, USBMODE, ABOUT };
Screen screen = HOME;
SPIClass sdBus(FSPI);
bool sdReady = false;
String currentPath = "/";
String entries[48];
bool entryIsDir[48];
int entryCount = 0, selected = 0, scroll = 0, tunerInstrument = 0;
int pianoOctave=0,pianoEffect=0,pianoVolume=85;
Preferences preferences;
String savedSsid, savedPassword, wifiEntry;
uint8_t wifiConfigStage=0;
int voiceEffect=0, voiceGain=2;
uint32_t tunerLastSample=0;
float tunerFrequency=0.0f, tunerCents=0.0f;
int tunerMidi=-1;
int16_t tunerSamples[2048];
int16_t voiceInput[512], voiceOutput[512], voiceDelay[8192];
size_t voiceDelayPos=0;
uint32_t voicePhase=0;
bool usbActive=false, usbHostEjected=false;
int8_t synthBuffer[2048];
const char* effectNames[]={"CLEAN","OVERDRIVE","FUZZ","SQUARE"};
String selectedFile;
uint8_t* wavBuffer = nullptr;
size_t wavLength = 0;
bool audioStarted = false;
const char* apps[] = {"SYNTH", "TUNER", "MUSIC", "FILES", "PHOTOS", "MIC FX", "WI-FI", "USB DRIVE", "SYSTEM"};
constexpr int APP_COUNT = 9;
const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
const float guitarHz[] = {82.41f,110.00f,146.83f,196.00f,246.94f,329.63f};
const char* guitarNames[] = {"E2","A2","D3","G3","B3","E4"};
const float ukeHz[] = {196.00f,261.63f,329.63f,392.00f};
const char* ukeNames[] = {"G4","C4","E4","A4"};

#if JOHNNY_USB_MSC
SdFat32 usbSd;
USBMSC msc;
bool usbStarted=false;
int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (!usbSd.card() || !buffer) return 0;
  uint8_t sector[512], *out=(uint8_t*)buffer;
  uint32_t done=0;
  while(done<bufsize) {
    if(!usbSd.card()->readSector(lba,sector)) return (int32_t)done;
    uint32_t n=min((uint32_t)(512-offset),bufsize-done);
    memcpy(out+done,sector+offset,n);
    done+=n; lba++; offset=0;
  }
  return (int32_t)done;
}
int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (!usbSd.card() || !buffer) return 0;
  uint8_t sector[512];
  uint32_t done=0;
  while(done<bufsize) {
    uint32_t n=min((uint32_t)(512-offset),bufsize-done);
    if(offset!=0 || n!=512) if(!usbSd.card()->readSector(lba,sector)) return (int32_t)done;
    memcpy(sector+offset,buffer+done,n);
    if(!usbSd.card()->writeSector(lba,sector)) return (int32_t)done;
    done+=n; lba++; offset=0;
  }
  return (int32_t)done;
}
bool mscStartStop(uint8_t, bool start, bool loadEject) {
  if(loadEject) { usbHostEjected=!start; msc.mediaPresent(start); }
  return true;
}
#endif

}

void header(const char* title) {
  auto &d=M5Cardputer.Display;
  d.fillScreen(BG);
  d.setTextSize(1);
  d.setTextColor(UI_PINK,BG); d.setCursor(7,5); d.print("JOHNNY");
  d.drawFastHLine(7,18,226,UI_PURPLE);
  d.setTextSize(2); d.setTextColor(WHITE,BG); d.setCursor(8,25); d.print(title);
  d.setTextSize(1);
}
void footer(const char* s="ESC / '  BACK") {
  auto &d=M5Cardputer.Display;
  d.drawFastHLine(7,117,226,0x5140);
  d.setTextColor(UI_MUTED,BG); d.setCursor(8,124); d.print(s);
}
void drawHome() {
  header("JOHNNY");
  auto &d=M5Cardputer.Display;
  d.setTextColor(UI_PINK,BG); d.setCursor(8,29); d.print("WAKE THE FUCK UP, SAMURAI.");
  const int xs[3]={5,83,161}, ys[3]={43,68,93};
  for(int i=0;i<APP_COUNT;i++){
    int col=i%3,row=i/3,x=xs[col],y=ys[row]; bool on=i==selected;
    uint16_t fill=on?0x5148:PANEL;
    d.fillRoundRect(x,y,73,22,3,fill);
    d.drawRoundRect(x,y,73,22,3,on?UI_PINK:UI_PURPLE);
    d.setTextColor(on?WHITE:UI_MUTED,fill);
    d.setCursor(x+5,y+8); d.printf("%s",apps[i]);
  }
  footer(", . / ;  ARROWS  ENTER");
}
void splash() {
  auto &d=M5Cardputer.Display;
  d.fillScreen(BG);
  d.setTextSize(3);
  d.setTextColor(UI_PURPLE,BG);
  d.setCursor(38,48); d.print("JOHNNY");
  delay(350);
  for(int i=0;i<10;i++) {
    int y=random(43,87), x=random(12,45), w=random(70,205);
    d.fillRect(x,y,w,2,i%2?UI_PINK:UI_CYAN);
    d.setTextColor(i%2?UI_PINK:UI_CYAN,BG);
    d.setCursor(37+random(-4,5),48+random(-2,3)); d.print("JOHNNY");
    delay(32);
  }
  d.fillScreen(BG);
  d.setTextColor(WHITE,BG); d.setCursor(38,48); d.print("JOHNNY");
  delay(650);
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
  d.setTextColor(UI_CYAN,BG); d.setCursor(8,108); d.print("- / + VOLUME   [ / ] OCTAVE");
  footer(", . / ; ARROWS   UP/DOWN TIMBRE");
}
void playNote(char c) {
  const char* keys="qwertyu";
  const char* pos=strchr(keys,(char)tolower((unsigned char)c));
  if(!pos)return;
  const int scale[7]={0,2,4,5,7,9,11};
  int midi=60+pianoOctave*12+scale[(int)(pos-keys)];
  float hz=440.0f*powf(2.0f,(midi-69)/12.0f);
  M5Cardputer.Speaker.stop();
  for(int i=0;i<2048;i++){
    float phase=6.2831853f*hz*((float)i/22050.0f);
    float v=sinf(phase);
    if(pianoEffect==1)v=tanhf(v*3.8f)/tanhf(3.8f);
    else if(pianoEffect==2)v=tanhf((sinf(phase)+0.55f*sinf(phase*2)+0.28f*sinf(phase*3))*5.0f);
    else if(pianoEffect==3)v=(v>=0)?0.72f:-0.72f;
    float envelope=1.0f-(float)i/2048.0f; envelope*=envelope;
    v*=envelope; synthBuffer[i]=(int8_t)(v*(float)pianoVolume);
  }
  M5Cardputer.Speaker.playRaw(synthBuffer,2048,22050,false,1,0,true);
}
bool captureMic(int16_t* dst, size_t count, uint32_t timeoutMs=260) {
  if(!M5Cardputer.Mic.begin()) return false;
  bool queued=M5Cardputer.Mic.record(dst,count,16000,false);
  if(!queued) { M5Cardputer.Mic.end(); return false; }
  uint32_t started=millis();
  while(M5Cardputer.Mic.isRecording() && millis()-started<timeoutMs) delay(1);
  bool complete=!M5Cardputer.Mic.isRecording();
  M5Cardputer.Mic.end();
  return complete;
}
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
void tunerScreen() {
  header("CHROMATIC TUNER");
  auto &d=M5Cardputer.Display;
  d.setTextColor(tunerInstrument==0?UI_PINK:UI_MUTED,BG); d.setCursor(8,43); d.print("GUITAR");
  d.setTextColor(tunerInstrument==1?UI_PINK:UI_MUTED,BG); d.setCursor(152,43); d.print("UKULELE");
  if(tunerMidi>=0) {
    int note=((tunerMidi%12)+12)%12, octave=tunerMidi/12-1;
    d.setTextSize(3); d.setTextColor(UI_CYAN,BG); d.setCursor(9,61); d.printf("%s%d",noteNames[note],octave);
    d.setTextSize(1); d.setTextColor(WHITE,BG); d.setCursor(146,65); d.printf("%.1f Hz",tunerFrequency);
    d.setTextColor(fabsf(tunerCents)<5?UI_CYAN:UI_PINK,BG); d.setCursor(146,82); d.printf("%+.0f cents",tunerCents);
    d.drawFastHLine(16,101,208,UI_PURPLE); d.drawFastVLine(120,96,11,UI_CYAN);
    int marker=120+(int)constrain(tunerCents,-50.0f,50.0f)*2;
    d.fillCircle(marker,101,3,UI_PINK);
  } else {
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,70); d.print("PLAY A SINGLE NOTE");
    d.setCursor(8,85); d.print("LISTENING TO INTERNAL MIC...");
  }
  const char* ref=tunerInstrument==0?"E2 A2 D3 G3 B3 E4":"G4 C4 E4 A4";
  d.setTextColor(UI_MUTED,BG); d.setCursor(8,111); d.print(ref);
  footer(", / ; CHANGE INSTRUMENT");
}

void drawNetworkScreen() {
  header("WI-FI");
  auto &d=M5Cardputer.Display;
  if(wifiConfigStage==1 || wifiConfigStage==2) {
    d.setTextColor(UI_CYAN,BG); d.setCursor(8,47);
    d.print(wifiConfigStage==1?"NETWORK NAME":"PASSWORD");
    d.setTextColor(WHITE,BG); d.setCursor(8,66);
    String shown=wifiEntry;
    if(wifiConfigStage==2) { shown=""; for(size_t i=0;i<wifiEntry.length();i++) shown+="*"; }
    if(shown.length()>29)shown=shown.substring(shown.length()-29);
    d.print(shown);
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,88); d.print("TYPE THEN PRESS ENTER");
    footer("ESC / '  CANCEL");
    return;
  }
  if(WiFi.status()==WL_CONNECTED) {
    d.setTextColor(UI_CYAN,BG); d.setCursor(8,48); d.print("CONNECTED");
    d.setTextColor(WHITE,BG); d.setCursor(8,66); d.print(WiFi.SSID());
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,82); d.print(WiFi.localIP().toString());
    d.setCursor(8,98); d.printf("SIGNAL %d dBm",WiFi.RSSI());
  } else {
    d.setTextColor(UI_PINK,BG); d.setCursor(8,54); d.print("NOT CONNECTED");
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,75); d.print(savedSsid.length()?savedSsid:"NO NETWORK SAVED");
  }
  footer("ENTER CONFIGURE  ESC / ' BACK");
}
void startNetworkSetup() {
  wifiConfigStage=1; wifiEntry="";
  drawNetworkScreen();
}
void saveNetworkSetup() {
  if(wifiConfigStage!=2) return;
  savedPassword=wifiEntry;
  preferences.putString("ssid",savedSsid);
  preferences.putString("pass",savedPassword);
  wifiConfigStage=0;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.begin(savedSsid.c_str(),savedPassword.c_str());
  drawNetworkScreen();
}
void drawVoiceScreen() {
  header("MIC FX");
  auto &d=M5Cardputer.Display;
  const char* names[]={"CLEAN","ECHO","ROBOT","FUZZ"};
  d.setTextColor(UI_PINK,BG); d.setCursor(8,49); d.print(names[voiceEffect]);
  d.setTextColor(UI_CYAN,BG); d.setCursor(8,68); d.printf("GAIN %d",voiceGain);
  d.setTextColor(UI_MUTED,BG); d.setCursor(8,87); d.print("LIVE MIC -> SPEAKER");
  d.setCursor(8,101); d.print("HALF-DUPLEX MONITOR");
  footer(",./; FX   -/+ GAIN");
}
void runVoiceChunk() {
  if(M5Cardputer.Speaker.isPlaying()) return;
  if(!captureMic(voiceInput,512,180)) return;
  M5Cardputer.Speaker.setVolume(170);
  for(size_t i=0;i<512;i++) {
    int32_t dry=(int32_t)voiceInput[i]*voiceGain, wet=dry;
    if(voiceEffect==1) {
      int32_t delayed=voiceDelay[voiceDelayPos];
      wet=dry+(delayed*55)/100;
      voiceDelay[voiceDelayPos]=(int16_t)constrain(dry+(delayed*35)/100,-32768,32767);
      voiceDelayPos=(voiceDelayPos+1)%8192;
    } else if(voiceEffect==2) {
      float mod=(voicePhase++%64)<32?1.0f:-1.0f;
      wet=(int32_t)(dry*mod*0.72f);
    } else if(voiceEffect==3) {
      float x=constrain((float)dry/12000.0f,-5.0f,5.0f);
      wet=(int32_t)(tanhf(x)*22000.0f);
    }
    voiceOutput[i]=(int16_t)constrain(wet,-32768,32767);
  }
  M5Cardputer.Speaker.playRaw(voiceOutput,512,16000,false,1,0,true);
}
#if JOHNNY_USB_MSC
bool beginUsbMode() {
  if(!sdReady) return false;
  if(audioStarted) { M5Cardputer.Speaker.stop(); audioStarted=false; }
  if(wavBuffer) { free(wavBuffer); wavBuffer=nullptr; wavLength=0; }
  SD.end(); sdReady=false; sdBus.end();
  sdBus.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  if(!usbSd.cardBegin(SdSpiConfig(SD_CS,DEDICATED_SPI,SD_SCK_MHZ(20),&sdBus))) {
    usbSd.end(); sdBus.end(); sdBus.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
    sdReady=SD.begin(SD_CS,sdBus,20000000); return false;
  }
  msc.vendorID("JOHNNY"); msc.productID("CARDPUTER SD"); msc.productRevision("1.0");
  msc.onRead(mscRead); msc.onWrite(mscWrite); msc.onStartStop(mscStartStop);
  uint32_t sectors=(uint32_t)usbSd.card()->sectorCount();
  if(!msc.begin(sectors,512)) {
    usbSd.end(); sdBus.end(); sdBus.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
    sdReady=SD.begin(SD_CS,sdBus,20000000); return false;
  }
  msc.mediaPresent(true); usbHostEjected=false;
  if(!usbStarted) { USB.begin(); usbStarted=true; }
  usbActive=true;
  return true;
}
void endUsbMode() {
  msc.mediaPresent(false);
  delay(80);
  usbSd.end();
  sdBus.end(); sdBus.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  sdReady=SD.begin(SD_CS,sdBus,20000000);
  usbActive=false;
}
#else
bool beginUsbMode() { return false; }
void endUsbMode() {}
#endif
void drawUsbScreen(bool ok) {
  header("USB DRIVE");
  auto &d=M5Cardputer.Display;
  if(ok) {
    d.setTextColor(UI_CYAN,BG); d.setCursor(8,52); d.print("SD CARD EXPORTED");
    d.setTextColor(WHITE,BG); d.setCursor(8,70); d.print("PC CAN READ / WRITE");
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,88); d.print("EJECT BEFORE DISCONNECT");
  } else {
    d.setTextColor(UI_PINK,BG); d.setCursor(8,52); d.print("USB MSC UNAVAILABLE");
    d.setTextColor(UI_MUTED,BG); d.setCursor(8,70); d.print("CHECK SD CARD / USB MODE");
  }
  footer("ESC / '  RETURN TO JOHNNY");
}
String parentPath(const String &p) {
  if(p=="/") return "/";
  int n=p.lastIndexOf('/');
  return n<=0?"/":p.substring(0,n);
}
bool isPlayableAudio(String name) {
  name.toLowerCase();
  return name.endsWith(".wav");
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
  for(int i=0;i<entryCount-1;i++) for(int j=i+1;j<entryCount;j++)
    if((!entryIsDir[i] && entryIsDir[j]) || (entryIsDir[i]==entryIsDir[j] && entries[j].compareTo(entries[i])<0)) {
      String t=entries[i]; entries[i]=entries[j]; entries[j]=t;
      bool b=entryIsDir[i]; entryIsDir[i]=entryIsDir[j]; entryIsDir[j]=b;
    }
}
void drawFiles(bool musicMode=false) {
  header(musicMode?"MUSIC LIBRARY":"SD EXPLORER");
  auto &d=M5Cardputer.Display;
  d.setTextColor(UI_CYAN,BG); d.setCursor(8,42);
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
    d.setTextColor(on?WHITE:(entryIsDir[i]?UI_CYAN:UI_MUTED),on?PANEL:BG);
    d.setCursor(12,y);
    String n=entries[i]; if(n.length()>29)n=n.substring(0,26)+"...";
    d.printf("%c %s%s",on?'>':' ',n.c_str(),entryIsDir[i]?"/":"");
    shown++;
  }
  if(entryCount==0){d.setTextColor(UI_MUTED,BG);d.setCursor(10,68);d.print("EMPTY DIRECTORY");}
  footer("ARROWS MOVE  ENTER OPEN  BACKSPACE UP");
}
bool startWav(const String &path) {
  if (wavBuffer) { M5Cardputer.Speaker.stop(); free(wavBuffer); wavBuffer=nullptr; wavLength=0; }
  File f=SD.open(path, FILE_READ);
  if(!f || f.isDirectory()) { if(f)f.close(); return false; }
  size_t size=f.size();
  if(size<44 || size>1500000) { f.close(); return false; }
  wavBuffer=(uint8_t*)ps_malloc(size);
  if(!wavBuffer) wavBuffer=(uint8_t*)malloc(size);
  if(!wavBuffer) { f.close(); return false; }
  wavLength=f.read(wavBuffer,size); f.close();
  if(wavLength!=size || memcmp(wavBuffer,"RIFF",4)!=0 || memcmp(wavBuffer+8,"WAVE",4)!=0) {
    free(wavBuffer); wavBuffer=nullptr; wavLength=0; return false;
  }
  audioStarted=M5Cardputer.Speaker.playWav(wavBuffer,wavLength,1,-1,true);
  return audioStarted;
}
void drawMusicInfo(const String &name) {
  header("NOW SELECTED");
  auto &d=M5Cardputer.Display;
  d.fillRoundRect(69,43,100,54,5,PANEL);
  d.setTextColor(UI_PINK,PANEL); d.setTextSize(3); d.setCursor(105,58); d.print("♪");
  d.setTextSize(1); d.setTextColor(WHITE,BG); d.setCursor(8,103);
  String n=name; if(n.length()>34)n=n.substring(0,31)+"...";
  d.print(n);
  d.setTextColor(UI_MUTED,BG); d.setCursor(8,113); d.print(audioStarted?"WAV PLAYING":"WAV ONLY / SELECT TO PLAY");
  footer("ESC  RETURN TO LIBRARY");
}
void openFile(bool musicMode) {
  if(!sdReady)return;
  if(selected<0 || selected>=entryCount)return;
  String full=currentPath;
  if(full!="/")full+="/";
  full+=entries[selected];
  if(entryIsDir[selected]) {currentPath=full;scanDirectory(musicMode);drawFiles(musicMode);return;}
  selectedFile=full;
  if(musicMode) {
    String lower=entries[selected]; lower.toLowerCase();
    bool ok=lower.endsWith(".wav") && startWav(full);
    drawMusicInfo(entries[selected]);
    if(!ok) { M5Cardputer.Display.setTextColor(UI_PINK,BG); M5Cardputer.Display.setCursor(8,101); M5Cardputer.Display.print("Use PCM WAV (max 1.5 MB)"); }
  }
  else {
    File f=SD.open(full);
    header("FILE DETAILS");
    auto &d=M5Cardputer.Display;
    d.setTextColor(WHITE,BG);d.setCursor(8,53);d.print(entries[selected].substring(0,30));
    if(f) {
      d.setTextColor(UI_MUTED,BG);d.setCursor(8,72);d.printf("%lu bytes",(unsigned long)f.size());
      d.setCursor(8,88);d.print("TYPE: ");d.print(f.isDirectory()?"DIR":"FILE");
      f.close();
    } else {
      d.setTextColor(UI_PINK,BG);d.setCursor(8,72);d.print("UNABLE TO OPEN FILE");
    }
    footer();
  }
}
void openApp() {
  switch(selected) {
    case 0:screen=PIANO;pianoScreen();break;
    case 1:screen=TUNER;tunerScreen();break;
    case 2:screen=MUSIC;currentPath="/";scanDirectory(true);drawFiles(true);break;
    case 3:screen=FILES;currentPath="/";scanDirectory(false);drawFiles();break;
    case 4:screen=GALLERY;header("PHOTO VIEWER");M5Cardputer.Display.setTextColor(UI_MUTED,BG);M5Cardputer.Display.setCursor(8,61);M5Cardputer.Display.print("JPEG decoder not included");footer();break;
    default:screen=ABOUT;header("SYSTEM");M5Cardputer.Display.setTextColor(WHITE,BG);M5Cardputer.Display.setCursor(8,53);M5Cardputer.Display.print("JOHNNY OS / CARDPUTER-ADV");M5Cardputer.Display.setCursor(8,69);M5Cardputer.Display.print("ESP32-S3  |  8 MB FLASH");M5Cardputer.Display.setCursor(8,85);M5Cardputer.Display.printf("SD: %s",sdReady?"MOUNTED":"NOT FOUND");footer();break;
  }
}
void setup() {
  auto cfg=M5.config(); M5Cardputer.begin(cfg,true);
  M5Cardputer.Display.setRotation(1);
  pinMode(38,OUTPUT); digitalWrite(38,HIGH);
  randomSeed(esp_random());
  sdBus.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  sdReady=SD.begin(SD_CS,sdBus,20000000);
  splash(); selected=0; drawHome();
}
void loop() {
  M5Cardputer.update();
  if(audioStarted && !M5Cardputer.Speaker.isPlaying()) { audioStarted=false; if(wavBuffer){free(wavBuffer);wavBuffer=nullptr;wavLength=0;} }
  if(!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()){delay(8);return;}
  auto k=M5Cardputer.Keyboard.keysState();
  char c=k.word.size()?k.word[0]:0;
  if(screen==HOME) {
    if(k.enter){openApp();}
    else if(k.up||c==';'){selected=(selected+APP_COUNT-2)%APP_COUNT;drawHome();}
    else if(k.down||c=='.'){selected=(selected+2)%APP_COUNT;drawHome();}
    else if(k.left||c==','){selected=(selected+APP_COUNT-1)%APP_COUNT;drawHome();}
    else if(k.right||c=='/'){selected=(selected+1)%APP_COUNT;drawHome();}
  } else if(screen==PIANO) {
    if(k.left||c==','){pianoOctave=max(-2,pianoOctave-1);pianoScreen();}
    else if(k.right||c=='/'){pianoOctave=min(2,pianoOctave+1);pianoScreen();}
    else if(k.up||c==';'){pianoEffect=(pianoEffect+3)%4;pianoScreen();}
    else if(k.down||c=='.'){pianoEffect=(pianoEffect+1)%4;pianoScreen();}
    else if(c=='-') {pianoVolume=max(20,pianoVolume-10);pianoScreen();}
    else if(c=='='||c=='+') {pianoVolume=min(120,pianoVolume+10);pianoScreen();}
    else if(c=='[') {pianoOctave=max(-2,pianoOctave-1);pianoScreen();}
    else if(c==']') {pianoOctave=min(2,pianoOctave+1);pianoScreen();}
    else if(c)playNote(c);
  } else if(screen==TUNER) {
    if(k.left||k.right||c==','||c=='/'){tunerInstrument=1-tunerInstrument;tunerScreen();}
  } else if(screen==FILES || screen==MUSIC) {
    bool music=screen==MUSIC;
    if(k.up||c==';'||k.left||c==','){if(selected>0)selected--;if(selected<scroll)scroll=selected;drawFiles(music);}
    else if(k.down||c=='.'||k.right||c=='/'){if(selected<entryCount-1)selected++;if(selected>=scroll+5)scroll=selected-4;drawFiles(music);}
    else if(k.enter)openFile(music);
    else if(k.del){if(audioStarted){M5Cardputer.Speaker.stop();audioStarted=false;}if(wavBuffer){free(wavBuffer);wavBuffer=nullptr;wavLength=0;}if(currentPath!="/"){currentPath=parentPath(currentPath);scanDirectory(music);drawFiles(music);}else{screen=HOME;drawHome();}}
  }
  if(k.del && screen!=HOME && screen!=FILES && screen!=MUSIC){screen=HOME;drawHome();}
  delay(12);
}
