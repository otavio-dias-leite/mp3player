#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ctype.h>

namespace {
constexpr int SD_CS = 12, SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14;
constexpr uint16_t BG = 0x1008, PANEL = 0x280C, UI_PURPLE = 0xA01F, UI_PINK = 0xF19F, UI_LAVENDER = 0xD5BF, UI_MUTED = 0xB5B4;
enum Screen : uint8_t { HOME, PIANO, TUNER, FILES, MUSIC, NETWORK, USBMODE, ABOUT };
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
int lastWiFiStatus=-1;
String wifiNetworks[20];
int wifiNetworkRSSI[20];
int wifiNetworkAuth[20];
int wifiNetworkCount=0, wifiSelected=0, wifiScroll=0;
bool wifiScanPending=false;
uint32_t tunerLastSample=0;
float tunerFrequency=0.0f, tunerCents=0.0f;
int tunerMidi=-1;
int16_t tunerSamples[2048];
bool usbActive=false;
String usbCommandLine;
bool micStarted=false;
uint32_t micWarmupUntil=0;
int8_t synthBuffer[2048];
const char* effectNames[]={"CLEAN","OVERDRIVE","FUZZ","SQUARE"};
String selectedFile;
uint8_t* wavBuffer = nullptr;
size_t wavLength = 0;
bool audioStarted = false;
uint32_t batteryLastDraw = 0;
const char* apps[] = {"SYNTH", "TUNER", "MUSIC", "FILES", "WI-FI", "USB", "SYSTEM"};
constexpr int APP_COUNT = 7;
const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
const float guitarHz[] = {82.41f,110.00f,146.83f,196.00f,246.94f,329.63f};
const char* guitarNames[] = {"E2","A2","D3","G3","B3","E4"};
const float ukeHz[] = {196.00f,261.63f,329.63f,392.00f};
const char* ukeNames[] = {"G4","C4","E4","A4"};


}

int batteryPercent() { return M5Cardputer.Power.getBatteryLevel(); }
uint16_t batteryMillivolts() { return M5Cardputer.Power.getBatteryVoltage(); }
bool batteryCharging() { return M5Cardputer.Power.isCharging(); }
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
  if(charging) { d.setTextColor(UI_PINK,BG); d.setCursor(225,5); d.print("+"); }
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
  d.setTextColor(UI_LAVENDER,BG); d.setCursor(8,108); d.print("- / + VOLUME   [ / ] OCTAVE");
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
  M5Cardputer.Speaker.setVolume(255);
  const float outputGain=(float)pianoVolume*1.27f;
  for(int i=0;i<2048;i++){
    float phase=6.2831853f*hz*((float)i/22050.0f);
    float v=sinf(phase);
    if(pianoEffect==1)v=tanhf(v*3.8f)/tanhf(3.8f);
    else if(pianoEffect==2)v=tanhf((sinf(phase)+0.55f*sinf(phase*2)+0.28f*sinf(phase*3))*5.0f);
    else if(pianoEffect==3)v=(v>=0)?0.72f:-0.72f;
    float envelope=1.0f-(float)i/2048.0f; envelope*=envelope;
    v*=envelope; synthBuffer[i]=(int8_t)constrain((int)lroundf(v*outputGain),-127,127);
  }
  M5Cardputer.Speaker.playRaw(synthBuffer,2048,22050,false,1,0,true);
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
  d.setTextColor(tunerInstrument==0?UI_PINK:UI_MUTED,BG); d.setCursor(8,43); d.print("GUITAR");
  d.setTextColor(tunerInstrument==1?UI_PINK:UI_MUTED,BG); d.setCursor(152,43); d.print("UKULELE");
  if(tunerMidi>=0) {
    int note=((tunerMidi%12)+12)%12, octave=tunerMidi/12-1;
    d.setTextSize(3); d.setTextColor(UI_LAVENDER,BG); d.setCursor(9,61); d.printf("%s%d",noteNames[note],octave);
    d.setTextSize(1); d.setTextColor(WHITE,BG); d.setCursor(146,65); d.printf("%.1f Hz",tunerFrequency);
    d.setTextColor(fabsf(tunerCents)<5?UI_LAVENDER:UI_PINK,BG); d.setCursor(146,82); d.printf("%+.0f cents",tunerCents);
    d.drawFastHLine(16,101,208,UI_PURPLE); d.drawFastVLine(120,96,11,UI_LAVENDER);
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
  if(audioStarted) { M5Cardputer.Speaker.stop(); audioStarted=false; }
  if(wavBuffer) { free(wavBuffer); wavBuffer=nullptr; wavLength=0; }
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
  footer("ESC / '  RETURN TO LIBRARY");
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
    case 1:screen=TUNER;tunerMidi=-1;tunerFrequency=0;tunerLastSample=0;tunerScreen();break;
    case 2:screen=MUSIC;currentPath="/";scanDirectory(true);drawFiles(true);break;
    case 3:screen=FILES;currentPath="/";scanDirectory(false);drawFiles();break;
    case 4:screen=NETWORK;wifiConfigStage=0;lastWiFiStatus=-1;drawNetworkScreen();break;
    case 5:screen=USBMODE;drawUsbScreen(beginUsbMode());break;
    default:
      screen=ABOUT; header("SYSTEM");
      M5Cardputer.Display.setTextColor(WHITE,BG); M5Cardputer.Display.setCursor(8,53); M5Cardputer.Display.print("JOHNNY / CARDPUTER-ADV");
      M5Cardputer.Display.setCursor(8,69); M5Cardputer.Display.print("ESP32-S3  |  8 MB FLASH");
      M5Cardputer.Display.setCursor(8,85); M5Cardputer.Display.printf("SD: %s",sdReady?"MOUNTED":"NOT FOUND");
      M5Cardputer.Display.setCursor(8,91); M5Cardputer.Display.printf("WI-FI: %s",WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE");
      int battery=batteryPercent(); uint16_t mv=batteryMillivolts(); bool charging=batteryCharging();
      M5Cardputer.Display.setCursor(8,103);
      if(battery>=0) M5Cardputer.Display.printf("BATTERY: %d%%  %.2fV",constrain(battery,0,100),mv/1000.0f);
      else M5Cardputer.Display.print("BATTERY: SENSOR UNAVAILABLE");
      M5Cardputer.Display.setCursor(8,115);
      M5Cardputer.Display.setTextColor(charging?UI_PINK:UI_MUTED,BG);
      M5Cardputer.Display.print(charging?"CHARGING  [USB POWER]":(battery<=15?"LOW BATTERY":"ON BATTERY"));
      footer("ESC / ' BACK"); break;
  }
}
void setup() {
  auto cfg=M5.config();
  M5Cardputer.begin(cfg,true);
  M5Cardputer.Display.setRotation(1);
  pinMode(38,OUTPUT); digitalWrite(38,HIGH);
  randomSeed(esp_random());
  sdBus.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  sdReady=SD.begin(SD_CS,sdBus,20000000);
  Serial.begin(115200);
  preferences.begin("johnny",false);
  savedSsid=preferences.getString("ssid","");
  savedPassword=preferences.getString("pass","");
  if(savedSsid.length()) {
    WiFi.mode(WIFI_STA); WiFi.setSleep(false);
    WiFi.begin(savedSsid.c_str(),savedPassword.c_str());
  }
  auto micCfg=M5Cardputer.Mic.config();
  micCfg.sample_rate=16000;
  M5Cardputer.Mic.config(micCfg);
  splash(); selected=0; drawHome();
}
void loop() {
  M5Cardputer.update();
  if(millis()-batteryLastDraw>=5000) {
    batteryLastDraw=millis();
    if(screen!=USBMODE) { drawBatteryIndicator(); if(screen==ABOUT) { header("SYSTEM"); M5Cardputer.Display.setTextColor(WHITE,BG); M5Cardputer.Display.setCursor(8,53); M5Cardputer.Display.print("JOHNNY / CARDPUTER-ADV"); M5Cardputer.Display.setCursor(8,69); M5Cardputer.Display.print("ESP32-S3  |  8 MB FLASH"); M5Cardputer.Display.setCursor(8,85); M5Cardputer.Display.printf("SD: %s",sdReady?"MOUNTED":"NOT FOUND"); M5Cardputer.Display.setCursor(8,91); M5Cardputer.Display.printf("WI-FI: %s",WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE"); int battery=batteryPercent(); uint16_t mv=batteryMillivolts(); M5Cardputer.Display.setCursor(8,103); if(battery>=0) M5Cardputer.Display.printf("BATTERY: %d%%  %.2fV",constrain(battery,0,100),mv/1000.0f); else M5Cardputer.Display.print("BATTERY: SENSOR UNAVAILABLE"); M5Cardputer.Display.setCursor(8,115); M5Cardputer.Display.setTextColor(batteryCharging()?UI_PINK:UI_MUTED,BG); M5Cardputer.Display.print(batteryCharging()?"CHARGING  [USB POWER]":(battery<=15?"LOW BATTERY":"ON BATTERY")); footer("ESC / ' BACK"); } }
  }
  if(audioStarted && !M5Cardputer.Speaker.isPlaying()) {
    audioStarted=false;
    if(wavBuffer){free(wavBuffer);wavBuffer=nullptr;wavLength=0;}
  }
  if(screen==NETWORK && wifiConfigStage==0) {
    int status=(int)WiFi.status();
    if(status!=lastWiFiStatus) { lastWiFiStatus=status; drawNetworkScreen(); }
  }
  if(screen==NETWORK && wifiConfigStage==1 && wifiScanPending) {
    int result=WiFi.scanComplete();
    if(result>=0) {
      wifiNetworkCount=min(result,20);
      for(int i=0;i<wifiNetworkCount;i++) { wifiNetworks[i]=WiFi.SSID(i); wifiNetworkRSSI[i]=WiFi.RSSI(i); wifiNetworkAuth[i]=(int)WiFi.encryptionType(i); }
      wifiScanPending=false; WiFi.scanDelete();
      if(wifiSelected>=wifiNetworkCount) wifiSelected=max(0,wifiNetworkCount-1);
      drawNetworkScreen();
    } else if(result==WIFI_SCAN_FAILED) { wifiScanPending=false; drawNetworkScreen(); }
  }
  if(screen==TUNER && millis()-tunerLastSample>220) {
    tunerLastSample=millis();
    if(captureMic(tunerSamples,2048,320)) {
      float freq=detectPitch(tunerSamples,2048);
      if(freq>0 && freq<1400) { tunerFrequency=freq; tunerMidi=nearestMidi(freq,&tunerCents); }
      else { tunerFrequency=0; tunerMidi=-1; }
    } else { tunerFrequency=0; tunerMidi=-1; }
    tunerScreen();
  }
  
  if(Serial.available()>0) serialTransferTask();

  if(!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) { delay(4); return; }
  auto k=M5Cardputer.Keyboard.keysState();
  char c=k.word.size()?k.word[0]:0;
  bool back=(k.esc || c==0x27 || c=='`' || c=='~');
  if(back) {
    if(screen==FILES || screen==MUSIC) {
      bool music=screen==MUSIC;
      if(audioStarted){M5Cardputer.Speaker.stop();audioStarted=false;}
      if(wavBuffer){free(wavBuffer);wavBuffer=nullptr;wavLength=0;}
      if(currentPath!="/"){currentPath=parentPath(currentPath);scanDirectory(music);drawFiles(music);}
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
    if(k.up||c==';') row=(row+2)%3;
    else if(k.down||c=='.') row=(row+1)%3;
    else if(k.left||c==',') col=(col+2)%3;
    else if(k.right||c=='/') col=(col+1)%3;
    int candidate=row*3+col;
    if(candidate>=APP_COUNT) candidate=row*3+(APP_COUNT-1-row*3);
    selected=candidate;
    drawHome();
  } else if(screen==PIANO) {
    if(k.left||c==','){pianoOctave=max(-2,pianoOctave-1);pianoScreen();}
    else if(k.right||c=='/'){pianoOctave=min(2,pianoOctave+1);pianoScreen();}
    else if(k.up||c==';'){pianoEffect=(pianoEffect+3)%4;pianoScreen();}
    else if(k.down||c=='.'){pianoEffect=(pianoEffect+1)%4;pianoScreen();}
    else if(c=='-'){pianoVolume=max(0,pianoVolume-10);pianoScreen();}
    else if(c=='='||c=='+'){pianoVolume=min(100,pianoVolume+10);pianoScreen();}
    else if(c=='['){pianoOctave=max(-2,pianoOctave-1);pianoScreen();}
    else if(c==']'){pianoOctave=min(2,pianoOctave+1);pianoScreen();}
    else if(c)playNote(c);
  } else if(screen==TUNER) {
    if(k.left||k.right||c==','||c=='/'){tunerInstrument=1-tunerInstrument;tunerScreen();}
  } else if(screen==FILES || screen==MUSIC) {
    bool music=screen==MUSIC;
    if(k.up||c==';'||k.left||c==','){if(selected>0)selected--;if(selected<scroll)scroll=selected;drawFiles(music);}
    else if(k.down||c=='.'||k.right||c=='/'){if(selected<entryCount-1)selected++;if(selected>=scroll+5)scroll=selected-4;drawFiles(music);}
    else if(k.enter)openFile(music);
  } else if(screen==NETWORK) {
    if(wifiConfigStage==0) {
      if(k.enter) startNetworkSetup();
      else if(k.down||c=='.') {WiFi.disconnect(false,false);drawNetworkScreen();}
    } else if(wifiConfigStage==1) {
      if(c=='r'||c=='R') startNetworkSetup();
      else if(k.up||c==';') {if(wifiSelected>0)wifiSelected--;if(wifiSelected<wifiScroll)wifiScroll=wifiSelected;drawNetworkScreen();}
      else if(k.down||c=='.') {if(wifiSelected<wifiNetworkCount-1)wifiSelected++;if(wifiSelected>=wifiScroll+5)wifiScroll=wifiSelected-4;drawNetworkScreen();}
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