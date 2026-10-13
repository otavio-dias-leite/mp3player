#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <WiFi.h>
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


class JohnnyAudioOutput final : public AudioOutput {
public:
  explicit JohnnyAudioOutput(m5::Speaker_Class* speaker) : speaker_(speaker) {}
  bool begin() override { return speaker_ != nullptr; }
  bool ConsumeSample(int16_t sample[2]) override {
    if(!speaker_) return false;
    if(bufferIndex_ >= kBufferSamples) {
      flush();
      return false;
    }
    const int32_t l=Amplify(sample[LEFTCHANNEL]);
    const int32_t r=Amplify(sample[RIGHTCHANNEL]);
    buffers_[bufferSlot_][bufferIndex_++]=(int16_t)((l+r)/2);
    return true;
  }
  void flush() override {
    if(!speaker_ || bufferIndex_==0) return;
    uint32_t started=millis();
    while(speaker_->isPlaying() && (uint32_t)(millis()-started)<1000UL) delay(1);
    if(speaker_->isPlaying()) speaker_->stop();
    speaker_->playRaw(buffers_[bufferSlot_],bufferIndex_,hertz,false);
    bufferSlot_=(bufferSlot_+1)%3;
    bufferIndex_=0;
  }
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
};


namespace {
constexpr int SD_CS = 12, SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14;
constexpr uint16_t BG = 0x1008, PANEL = 0x280C, UI_PURPLE = 0xA01F, UI_PINK = 0xF19F, UI_LAVENDER = 0xD5BF, UI_MUTED = 0xB5B4;
enum Screen : uint8_t { HOME, PIANO, TUNER, FILES, MUSIC, RADIO, NETWORK, USBMODE, ABOUT };
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
int audioVolume=14;
bool audioPaused=false;
bool audioIsRadio=false;
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
bool audioStarted = false;
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
const char* apps[] = {"SYNTH", "TUNER", "MUSIC", "RADIO", "FILES", "WI-FI", "USB", "SYSTEM"};
constexpr int APP_COUNT = 8;
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
  d.fillRect(168,2,62,15,BG);
  d.setTextSize(1); d.setTextColor(level>=0 && level<=15?UI_PINK:UI_MUTED,BG);
  d.setCursor(169,6);
  if(level>=0) d.printf("%3d%%",constrain(level,0,100)); else d.print("--%");
  d.drawRoundRect(201,4,19,10,2,charging?UI_PINK:UI_LAVENDER);
  d.fillRect(220,7,2,4,charging?UI_PINK:UI_LAVENDER);
  if(level>=0) {
    int width=constrain(level,0,100)*13/100;
    if(width>0) d.fillRect(204,6,width,6,level<=15?UI_PINK:UI_LAVENDER);
  }
  if(charging) {
    d.drawLine(228,3,223,9,UI_PINK);
    d.drawLine(223,9,226,9,UI_PINK);
    d.drawLine(226,9,222,15,UI_PINK);
  }
}
void header(const char* title) {
  auto &d=M5Cardputer.Display;
  d.fillScreen(BG);
  d.setTextSize(1);
  d.setTextColor(UI_PINK,BG); d.setCursor(7,5); d.print("JOHNNY");
  drawBatteryIndicator();
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
  const int xs[3]={5,83,161}, ys[3]={43,68,93};
  for(int i=0;i<APP_COUNT;i++){
    int col=i%3,row=i/3,x=xs[col],y=ys[row]; bool on=i==selected;
    uint16_t fill=on?0x4810:PANEL;
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
    d.fillRect(x,y,w,2,i%2?UI_PINK:UI_PURPLE);
    d.setTextColor(i%2?UI_PINK:UI_PURPLE,BG);
    d.setCursor(37+random(-4,5),48+random(-2,3)); d.print("JOHNNY");
    delay(32);
  }
  d.fillScreen(BG);
  d.setTextColor(WHITE,BG); d.setCursor(38,43); d.print("JOHNNY");
  d.setTextSize(1); d.setTextColor(UI_PINK,BG); d.setCursor(52,72); d.print("wake fuck up samurai");
  delay(1100);
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
  M5Cardputer.Speaker.stop();
  M5Cardputer.Speaker.setVolume(255);
  const float gain=(float)pianoVolume*1.27f;
  const float guitarDecay=0.30f+0.34f*(82.0f/max(hz,82.0f));
  for(size_t i=0;i<8192;i++) {
    const float phase=6.2831853f*hz*((float)i/22050.0f);
    float v=sinf(phase);
    if(pianoEffect==1) {
      v=tanhf(v*3.8f)/tanhf(3.8f);
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
    const float envelope=(i<80)?((float)i/80.0f):1.0f;
    v*=envelope;
    synthBuffer[i]=(int8_t)constrain((int)lroundf(v*gain),-127,127);
  }
  M5Cardputer.Speaker.playRaw(synthBuffer,8192,22050,false,1,0,true);
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
  const char* modeNames[]={"GUITAR","VIOLAO","UKULELE"};
  d.setTextColor(UI_PINK,BG); d.setCursor(8,43);
  d.printf("MODE: %s",modeNames[tunerInstrument]);
  d.setTextColor(UI_MUTED,BG); d.setCursor(166,43); d.print("<  >");
  if(!tunerHasSignal) {
    d.setTextColor(UI_LAVENDER,BG); d.setTextSize(2); d.setCursor(8,63); d.print("TOQUE UMA CORDA");
    d.setTextSize(1); d.setTextColor(UI_MUTED,BG); d.setCursor(8,88); d.print("MANTENHA A NOTA SOANDO");
    d.setCursor(8,101); d.print("A LEITURA FICA NA TELA POR 5s");
    footer(", /  TROCAR INSTRUMENTO");
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
  d.setTextColor(UI_MUTED,PANEL); d.setCursor(16,63); d.printf("CORDA %s  /  ALVO %.1f Hz",nameTable[nearest],hzTable[nearest]);
  d.setTextColor(WHITE,PANEL); d.setTextSize(2); d.setCursor(16,78); d.printf("%+.0f CENT",cents);
  d.setTextSize(1); d.setTextColor(UI_LAVENDER,PANEL); d.setCursor(156,83); d.printf("%.1f Hz",tunerFrequency);
  d.drawFastHLine(18,113,204,UI_PURPLE);
  d.drawFastVLine(120,109,9,UI_LAVENDER);
  int marker=120+(int)constrain(cents,-50.0f,50.0f)*2;
  d.fillCircle(marker,113,3,statusColor);
  d.setTextColor(UI_MUTED,BG); d.setCursor(8,101);
  if(cents < -5.0f) { d.setTextColor(UI_PINK,BG); d.setCursor(8,101); d.print("GRAVE DEMAIS  ->  APERTE"); }
  else if(cents > 5.0f) { d.setTextColor(UI_PINK,BG); d.setCursor(8,101); d.print("AGUDA DEMAIS  ->  AFROUXE"); }
  else { d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,101); d.print("AFINADA  /  MANTENHA"); }
  footer(", /  TROCAR INSTRUMENTO");
}
void drawNetworkScreen() {
  header("WI-FI");
  auto &d=M5Cardputer.Display;
  if(wifiConfigStage==1) {
    d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,43);
    d.print(wifiScanPending?"SCANNING NETWORKS...":"SELECT A NETWORK");
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
    footer("ESC / ' BACK TO NETWORKS");
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
  footer("ENTER SEARCH NETWORKS  ESC / ' BACK");
}
void startNetworkSetup() {
  WiFi.mode(WIFI_STA); WiFi.disconnect(false,false);
  wifiNetworkCount=0; wifiSelected=0; wifiScroll=0; wifiScanPending=true;
  wifiConfigStage=1; WiFi.scanDelete(); WiFi.scanNetworks(true,false);
  drawNetworkScreen();
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
  if(line=="PING") { Serial.println("JOHNNY/1"); return; }
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
    if(path.length()==0 || path=="/" || size>16UL*1024UL*1024UL) { usbReplyError("LIMIT"); return; }
    if(SD.exists(path)) SD.remove(path);
    File file=SD.open(path,FILE_WRITE);
    if(!file) { usbReplyError("CREATE"); return; }
    Serial.println("READY");
    uint32_t received=0, lastByte=millis();
    uint8_t buffer[512];
    while(received<size && millis()-lastByte<120000UL) {
      int available=Serial.available();
      if(available<=0) { delay(1); continue; }
      size_t n=(size_t)min((uint32_t)min(available,(int)sizeof(buffer)),size-received);
      int got=Serial.read(buffer,n);
      if(got>0) {
        size_t written=file.write(buffer,(size_t)got);
        received+=(uint32_t)written; lastByte=millis();
        if(written!=(size_t)got) break;
      }
    }
    file.flush(); file.close();
    if(received==size) Serial.println("OK");
    else { SD.remove(path); usbReplyError("INCOMPLETE"); }
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
  if(!sdReady) return false;
  stopAudio();
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
  footer("ESC / '  RETURN TO JOHNNY");
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
  for(int i=0;i<entryCount-1;i++) for(int j=i+1;j<entryCount;j++)
    if((!entryIsDir[i] && entryIsDir[j]) || (entryIsDir[i]==entryIsDir[j] && entries[j].compareTo(entries[i])<0)) {
      String t=entries[i]; entries[i]=entries[j]; entries[j]=t;
      bool b=entryIsDir[i]; entryIsDir[i]=entryIsDir[j]; entryIsDir[j]=b;
    }
}
void drawFiles(bool musicMode=false) {
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
  footer(",./; MOVE  ENTER OPEN  ESC / ' BACK");
}

void drawMusicInfo(const String &name);
void drawRadioScreen();
void stopAudio() {
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
AudioGenerator* createAudioDecoder(const String& extension) {
  if(extension=="mp3") return new AudioGeneratorMP3();
  if(extension=="wav") return new AudioGeneratorWAV();
  if(extension=="flac") return new AudioGeneratorFLAC();
  if(extension=="aac") return new AudioGeneratorAAC();
  if(extension=="ogg" || extension=="opus" || extension=="oga") return new AudioGeneratorOpus();
  return nullptr;
}
bool startAudioSource(AudioFileSource* source, const String& title, const String& extension, bool radio) {
  stopAudio();
  if(!source) { audioStatusMessage="SOURCE ERROR"; return false; }
  audioRawSource=source;
  audioInputBuffer=new AudioFileSourceBuffer(audioRawSource,8192);
  if(!audioInputBuffer) { audioStatusMessage="NO BUFFER MEMORY"; stopAudio(); return false; }
  audioOutput=new JohnnyAudioOutput(&M5Cardputer.Speaker);
  if(!audioOutput) { audioStatusMessage="NO AUDIO OUTPUT"; stopAudio(); return false; }
  audioOutput->SetGain((float)audioVolume/21.0f);
  audioDecoder=createAudioDecoder(extension);
  if(!audioDecoder) { audioStatusMessage="UNSUPPORTED FORMAT"; stopAudio(); return false; }
  if(!audioDecoder->begin(audioInputBuffer,audioOutput)) {
    audioStatusMessage=(extension=="ogg" || extension=="opus" || extension=="oga")?
      "OGG MUST BE OPUS":"DECODE / STREAM ERROR";
    stopAudio();
    return false;
  }
  audioNowTitle=title;
  audioIsRadio=radio;
  audioPaused=false;
  audioStarted=true;
  audioStatusMessage=radio?"LIVE RADIO":"PLAYING";
  M5Cardputer.Speaker.setVolume(255);
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
bool startLocalAudio(const String& path) {
  if(!sdReady) { audioStatusMessage="NO SD CARD"; return false; }
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
void serviceAudio() {
  if(!audioStarted || audioPaused || !audioDecoder) return;
  if(!audioDecoder->loop()) {
    const bool wasRadio=audioIsRadio;
    stopAudio();
    audioStatusMessage=wasRadio?"STREAM ENDED":"TRACK FINISHED";
    if(wasRadio) radioMessage=audioStatusMessage;
    if(!screenSaverActive) {
      if(screen==MUSIC && musicNowPlaying) drawMusicInfo(audioNowTitle);
      else if(screen==RADIO) drawRadioScreen();
    }
  }
}
void setAudioVolume(int value) {
  audioVolume=constrain(value,0,21);
  if(audioOutput) audioOutput->SetGain((float)audioVolume/21.0f);
}
void drawMusicInfo(const String &name) {
  header("JOHNNY MUSIC");
  auto &d=M5Cardputer.Display;
  d.setTextColor(UI_MUTED,BG); d.setCursor(8,42);
  d.print("NOW PLAYING");
  d.fillRoundRect(91,49,58,43,5,PANEL);
  d.drawCircle(120,70,19,UI_PURPLE);
  d.drawCircle(120,70,12,UI_PINK);
  d.drawCircle(120,70,5,UI_LAVENDER);
  int previous=-1, next=-1;
  for(int step=1;step<entryCount;step++) {
    int candidate=(selected-step+entryCount)%entryCount;
    if(!entryIsDir[candidate] && isPlayableAudio(entries[candidate])) {previous=candidate;break;}
  }
  for(int step=1;step<entryCount;step++) {
    int candidate=(selected+step)%entryCount;
    if(!entryIsDir[candidate] && isPlayableAudio(entries[candidate])) {next=candidate;break;}
  }
  d.setTextColor(UI_LAVENDER,BG);
  d.setCursor(8,59);
  if(previous>=0) {
    String title=entries[previous]; if(title.length()>8) title=title.substring(0,7)+"~";
    d.printf("< %s",title.c_str());
  } else d.print("< --");
  d.setCursor(171,59);
  if(next>=0) {
    String title=entries[next]; if(title.length()>8) title=title.substring(0,7)+"~";
    d.printf("> %s",title.c_str());
  } else d.print("> --");
  String label=name;
  if(label.length()>34) label=label.substring(0,31)+"...";
  d.setTextSize(1); d.setTextColor(WHITE,BG); d.setCursor(8,98);
  d.print(label);
  d.setTextColor(audioStarted?(audioPaused?UI_PINK:UI_LAVENDER):UI_MUTED,BG);
  d.setCursor(8,109);
  d.printf("%s  VOL %02d",audioStarted?(audioPaused?"PAUSED":"PLAYING"):audioStatusMessage.c_str(),audioVolume);
  footer("< > NEXT/PREV  ENTER PAUSE  +/- VOL");
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
  if(status=="SELECT A STATION" || status.length()==0) status=audioStatusMessage;
  d.print(status.substring(0,30));
  footer("UP/DN STATION  ENTER PLAY  SPACE PAUSE");
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
  if(entryCount<=0) return;
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
  if(full!="/") full+="/";
  full+=entries[selected];
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
void openApp() {
  switch(selected) {
    case 0: stopMicCapture(); stopAudio(); screen=PIANO; pianoScreen(); break;
    case 1:
      stopAudio(); stopMicCapture(); screen=TUNER; tunerInstrument=1;
      tunerMidi=-1; tunerFrequency=0; tunerFilteredFrequency=0; tunerHasSignal=false;
      tunerLastSignalAt=0; tunerLastSample=0; tunerScreen(); break;
    case 2:
      if(audioIsRadio) stopAudio();
      screen=MUSIC; musicNowPlaying=false; currentPath="/"; scanDirectory(true); drawFiles(true); break;
    case 3:
      if(!audioIsRadio && audioStarted) stopAudio();
      screen=RADIO; radioEditingUrl=false; radioSelected=0; drawRadioScreen(); break;
    case 4: screen=FILES; currentPath="/"; scanDirectory(false); drawFiles(); break;
    case 5: screen=NETWORK; wifiConfigStage=0; lastWiFiStatus=-1; drawNetworkScreen(); break;
    case 6: screen=USBMODE; drawUsbScreen(beginUsbMode()); break;
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
      footer("ESC / ' BACK"); break;
  }
}
void setup() {
  auto cfg=M5.config();
  M5Cardputer.begin(cfg,true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(255);
  pinMode(38,OUTPUT); digitalWrite(38,HIGH);
  randomSeed(esp_random());
  sdBus.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  sdReady=SD.begin(SD_CS,sdBus,20000000);
  Serial.begin(115200);
  preferences.begin("johnny",false);
  savedSsid=preferences.getString("ssid","");
  savedPassword=preferences.getString("pass","");
  radioEntry=preferences.getString("radio_url","");
  if(savedSsid.length()) {
    WiFi.mode(WIFI_STA); WiFi.setSleep(false);
    WiFi.begin(savedSsid.c_str(),savedPassword.c_str());
  }
  auto micCfg=M5Cardputer.Mic.config();
  micCfg.sample_rate=16000;
  M5Cardputer.Mic.config(micCfg);
  refreshBatteryCache(true);
  splash(); selected=0; drawHome();
}
void loop() {
  M5Cardputer.update();
  if(millis()-batteryLastSample>=30000UL) {
    batteryLastDraw=millis();
    refreshBatteryCache(true);
    if(screen!=USBMODE && !screenSaverActive) { drawBatteryIndicator(); if(screen==ABOUT) { header("SYSTEM"); M5Cardputer.Display.setTextColor(WHITE,BG); M5Cardputer.Display.setCursor(8,53); M5Cardputer.Display.print("JOHNNY / CARDPUTER-ADV"); M5Cardputer.Display.setCursor(8,69); M5Cardputer.Display.print("ESP32-S3  |  8 MB FLASH"); M5Cardputer.Display.setCursor(8,85); M5Cardputer.Display.printf("SD: %s",sdReady?"MOUNTED":"NOT FOUND"); M5Cardputer.Display.setCursor(8,91); M5Cardputer.Display.printf("WI-FI: %s",WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE"); int battery=batteryPercent(); int16_t mv=batteryMillivolts(); M5Cardputer.Display.setCursor(8,103); if(battery>=0 && mv>0) M5Cardputer.Display.printf("BATTERY: %d%%  %.2fV",constrain(battery,0,100),mv/1000.0f); else if(battery>=0) M5Cardputer.Display.printf("BATTERY: %d%%  VOLT N/A",constrain(battery,0,100)); else M5Cardputer.Display.print("BATTERY: SENSOR UNAVAILABLE"); M5Cardputer.Display.setCursor(8,115); M5Cardputer.Display.setTextColor(batteryCharging()?UI_PINK:UI_MUTED,BG); M5Cardputer.Display.print(batteryCharging()?"CHARGING  [USB POWER]":(battery>=0 && battery<=15?"LOW BATTERY":"ON BATTERY")); footer("ESC / ' BACK"); } }
  }
  serviceAudio();
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
    tunerScreen();
  }
  
  if(Serial.available()>0) serialTransferTask();

  if(!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) { delay(4); return; }
  auto k=M5Cardputer.Keyboard.keysState();
  char c=k.word.size()?k.word[0]:0;
  if(k.tab) {
    screenSaverActive=!screenSaverActive;
    if(screenSaverActive) M5Cardputer.Display.fillScreen(BLACK);
    else if(screen==HOME) drawHome();
    else if(screen==PIANO) pianoScreen();
    else if(screen==TUNER) tunerScreen();
    else if(screen==FILES) drawFiles(false);
    else if(screen==MUSIC) musicNowPlaying?drawMusicInfo(audioNowTitle):drawFiles(true);
    else if(screen==RADIO) drawRadioScreen();
    else if(screen==NETWORK) drawNetworkScreen();
    else if(screen==USBMODE) drawUsbScreen(usbActive);
    else if(screen==ABOUT) openApp();
    return;
  }
  if(screenSaverActive) {
    screenSaverActive=false;
    if(screen==HOME) drawHome();
    else if(screen==PIANO) pianoScreen();
    else if(screen==TUNER) tunerScreen();
    else if(screen==FILES) drawFiles(false);
    else if(screen==MUSIC) musicNowPlaying?drawMusicInfo(audioNowTitle):drawFiles(true);
    else if(screen==RADIO) drawRadioScreen();
    else if(screen==NETWORK) drawNetworkScreen();
    else if(screen==USBMODE) drawUsbScreen(usbActive);
    else if(screen==ABOUT) openApp();
    return;
  }

  bool back=(c==0x27 || c=='`' || c=='~');
  if(back) {
    if(screen==FILES || screen==MUSIC) {
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

  if(screen==HOME) {
    if(k.enter) { openApp(); delay(8); return; }
    int row=selected/3, col=selected%3;
    if(c==';') row=(row+2)%3;
    else if(c=='.') row=(row+1)%3;
    else if(c==',') col=(col+2)%3;
    else if(c=='/') col=(col+1)%3;
    int candidate=row*3+col;
    if(candidate>=APP_COUNT) candidate=row*3+(APP_COUNT-1-row*3);
    selected=candidate;
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
      if(c==',') moveMusicTrack(-1);
      else if(c=='/') moveMusicTrack(1);
      else if(k.enter||k.space||c==' ') {audioPaused=!audioPaused;drawMusicInfo(audioNowTitle);}
      else if(c=='p'||c=='P') {stopAudio();audioStatusMessage="STOPPED";drawMusicInfo(audioNowTitle);}
      else if(c=='-'||c=='_') {setAudioVolume(audioVolume-1);drawMusicInfo(audioNowTitle);}
      else if(c=='='||c=='+') {setAudioVolume(audioVolume+1);drawMusicInfo(audioNowTitle);}
      else if(c==';') moveMusicTrack(-1);
      else if(c=='.') moveMusicTrack(1);
    } else {
      if(c==';') {if(selected>0)selected--;if(selected<scroll)scroll=selected;drawFiles(true);}
      else if(c=='.') {if(selected<entryCount-1)selected++;if(selected>=scroll+5)scroll=selected-4;drawFiles(true);}
      else if(c==',') {if(currentPath!="/"){currentPath=parentPath(currentPath);scanDirectory(true);drawFiles(true);}}
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
          startRadioStream(radioEntry,"CUSTOM STREAM"); drawRadioScreen();
        } else {radioMessage="URL MUST START HTTP://";drawRadioScreen();}
      } else if(c==8||c==127) {if(radioEntry.length())radioEntry.remove(radioEntry.length()-1);drawRadioScreen();}
      else if(c&&isprint((unsigned char)c)&&radioEntry.length()<180) {radioEntry+=c;drawRadioScreen();}
    } else {
      if(c==';') {radioSelected=max(0,radioSelected-1);drawRadioScreen();}
      else if(c=='.') {radioSelected=min(RADIO_STATION_COUNT,radioSelected+1);drawRadioScreen();}
      else if(k.enter) {
        if(radioSelected<RADIO_STATION_COUNT) startRadioStream(radioStationUrls[radioSelected],radioStationNames[radioSelected]);
        else {radioEntry=preferences.getString("radio_url",radioEntry);radioEditingUrl=true;}
        drawRadioScreen();
      } else if(k.space&&audioIsRadio) {audioPaused=!audioPaused;drawRadioScreen();}
      else if(c=='p'||c=='P') {stopAudio();audioStatusMessage="STOPPED";radioMessage="STOPPED";drawRadioScreen();}
      else if(c=='-'||c=='_') {setAudioVolume(audioVolume-1);drawRadioScreen();}
      else if(c=='='||c=='+') {setAudioVolume(audioVolume+1);drawRadioScreen();}
    }

  } else if(screen==NETWORK) {
    if(wifiConfigStage==0) {
      if(k.enter) startNetworkSetup();
      else if(c=='.') {WiFi.disconnect(false,false);drawNetworkScreen();}
    } else if(wifiConfigStage==1) {
      if(c=='r'||c=='R') startNetworkSetup();
      else if(c==';') {if(wifiSelected>0)wifiSelected--;if(wifiSelected<wifiScroll)wifiScroll=wifiSelected;drawNetworkScreen();}
      else if(c=='.') {if(wifiSelected<wifiNetworkCount-1)wifiSelected++;if(wifiSelected>=wifiScroll+5)wifiScroll=wifiSelected-4;drawNetworkScreen();}
      else if(k.enter && !wifiScanPending) chooseNetwork(wifiSelected);
    } else if(wifiConfigStage==2) {
      if(k.enter) saveNetworkSetup();
      else if(c && isprint((unsigned char)c) && wifiEntry.length()<63) {wifiEntry+=c;drawNetworkScreen();}
    }
  } else if(screen==USBMODE) {
    if(k.enter && !usbActive) {screen=HOME;drawHome();}
  }
  delay(8);
}