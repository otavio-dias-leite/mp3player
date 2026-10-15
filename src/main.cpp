#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <AudioOutput.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include "johnny_version.h"

namespace {
constexpr int SD_CS=12, SD_SCK=40, SD_MISO=39, SD_MOSI=14;
constexpr uint16_t BLACK=0x0000, RED=0xF800, WHITE=0xFFFF, GREY=0x9CF3, PANEL=0x2800;
SPIClass sdSPI(FSPI);
bool sdReady=false, isPlaying=false, isPaused=false;
String files[80], currentTrack;
int fileCount=0, selected=0, firstVisible=0, volume=75;
uint8_t audioBuffer[4096];
AudioFileSourceSD *source=nullptr;
AudioFileSourceBuffer *buffered=nullptr;
AudioGeneratorMP3 *mp3=nullptr;

class CardputerAudioOutput : public AudioOutput {
  int16_t samples[2][384]{};
  size_t count=0;
  uint8_t active=0;
  bool flushSamples() {
    if (!count) return true;
    uint32_t started=millis();
    while (!M5Cardputer.Speaker.playRaw(samples[active], count, hertz, false, 1, 0, false)) {
      if (millis()-started>150) return false;
      delay(1);
    }
    active^=1; count=0; return true;
  }
public:
  bool begin() override { return true; }
  bool ConsumeSample(int16_t sample[2]) override {
    if (count>=384 && !flushSamples()) return false;
    int32_t mono=((int32_t)sample[0]+sample[1])/2;
    mono=mono*volume/100;
    samples[active][count++]=(int16_t)constrain(mono,-32768,32767);
    return true;
  }
  void flush() override { flushSamples(); }
  bool stop() override { count=0; M5Cardputer.Speaker.stop(); return true; }
};
CardputerAudioOutput *audioOut=nullptr;

bool isMp3(const String &name) { String lower=name; lower.toLowerCase(); return lower.endsWith(".mp3"); }
void stopPlayback() {
  if(mp3){if(mp3->isRunning())mp3->stop();delete mp3;mp3=nullptr;}
  if(buffered){delete buffered;buffered=nullptr;}
  if(source){source->close();delete source;source=nullptr;}
  if(audioOut){audioOut->stop();delete audioOut;audioOut=nullptr;}
  M5Cardputer.Speaker.stop(); isPlaying=false; isPaused=false;
}
void drawHeader(const char *title) {
  auto &d=M5Cardputer.Display;
  d.fillScreen(BLACK); d.fillRect(0,0,240,23,0x1800);
  d.setTextSize(1); d.setTextColor(RED,0x1800); d.setCursor(8,7); d.print("JOHNNY");
  d.setTextColor(GREY,0x1800); d.setCursor(194,7); d.print("MP3");
  d.drawFastHLine(8,25,224,RED); d.setTextSize(2);
  d.setTextColor(WHITE,BLACK); d.setCursor(8,33); d.print(title); d.setTextSize(1);
}
void drawLibrary() {
  drawHeader("MUSIC"); auto &d=M5Cardputer.Display;
  if(!sdReady){d.setTextColor(RED,BLACK);d.setCursor(12,67);d.print("MICROSD NOT FOUND");}
  else if(!fileCount){d.setTextColor(GREY,BLACK);d.setCursor(12,67);d.print("NO MP3 FILES");}
  for(int row=0;row<6 && firstVisible+row<fileCount;++row){
    int i=firstVisible+row,y=55+row*10;bool active=(i==selected);
    d.fillRect(8,y-1,224,10,active?PANEL:BLACK);
    d.setTextColor(active?RED:WHITE,active?PANEL:BLACK);
    String name=files[i];if(name.length()>31)name=name.substring(0,28)+"...";
    d.setCursor(13,y);d.printf("%c %s",active?'>':' ',name.c_str());
  }
  d.drawFastHLine(8,117,224,0x528A);d.setTextColor(GREY,BLACK);
  d.setCursor(8,124);d.print("UP/DOWN SELECT  ENTER PLAY");
}
void drawPlayer() {
  drawHeader("NOW PLAYING");auto &d=M5Cardputer.Display;
  d.fillRoundRect(10,53,220,45,5,PANEL);d.setTextColor(RED,BLACK);
  d.setCursor(19,61);d.print(isPaused?"PAUSED":"PLAYING");
  d.setTextColor(WHITE,BLACK);d.setCursor(19,80);
  String name=currentTrack;if(name.length()>29)name=name.substring(0,26)+"...";
  d.print(name);d.setTextColor(GREY,BLACK);d.setCursor(19,91);d.printf("VOLUME %d%%",volume);
  d.drawFastHLine(8,117,224,0x528A);d.setCursor(8,124);d.print("SPACE PAUSE  +/- VOLUME  ESC BACK");
}
void scanRoot() {
  fileCount=0;selected=0;firstVisible=0;if(!sdReady)return;
  File root=SD.open("/");if(!root||!root.isDirectory()){if(root)root.close();return;}
  File entry=root.openNextFile();
  while(entry&&fileCount<80){
    if(!entry.isDirectory()){String name=String(entry.name());int slash=name.lastIndexOf('/');
      if(slash>=0)name=name.substring(slash+1);if(isMp3(name)&&name.length())files[fileCount++]=name;}
    entry.close();entry=root.openNextFile();
  }root.close();
}
bool startTrack(const String &name) {
  stopPlayback();String path="/"+name;
  source=new AudioFileSourceSD();if(!source||!source->open(path.c_str())){stopPlayback();return false;}
  buffered=new AudioFileSourceBuffer(source,audioBuffer,sizeof(audioBuffer));
  audioOut=new CardputerAudioOutput();if(!buffered||!audioOut){stopPlayback();return false;}
  audioOut->SetGain(1.0f);mp3=new AudioGeneratorMP3();
  if(!mp3||!mp3->begin(buffered,audioOut)){stopPlayback();return false;}
  currentTrack=name;isPlaying=true;isPaused=false;drawPlayer();return true;
}
void changeSelection(int delta) {
  if(!fileCount)return;selected=constrain(selected+delta,0,fileCount-1);
  if(selected<firstVisible)firstVisible=selected;
  if(selected>=firstVisible+6)firstVisible=selected-5;drawLibrary();
}
}
void setup() {
  auto cfg=M5.config();M5Cardputer.begin(cfg,true);M5Cardputer.Display.setRotation(1);
  M5Cardputer.Speaker.begin();pinMode(38,OUTPUT);digitalWrite(38,HIGH);
  sdSPI.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);sdReady=SD.begin(SD_CS,sdSPI,16000000);
  scanRoot();drawLibrary();Serial.begin(115200);
  Serial.printf("JOHNNY MP3 %s SD=%s TRACKS=%d\n",JOHNNY_VERSION,sdReady?"OK":"MISSING",fileCount);
}
void loop() {
  M5Cardputer.update();
  if(isPlaying&&!isPaused&&mp3&&!mp3->loop()){stopPlayback();drawLibrary();}
  if(!M5Cardputer.Keyboard.isChange()||!M5Cardputer.Keyboard.isPressed()){delay(2);return;}
  auto keys=M5Cardputer.Keyboard.keysState();char key=keys.word.size()?keys.word[0]:0;
  if(isPlaying){
    if(keys.space||key==' '){isPaused=!isPaused;if(isPaused)mp3->stop();else mp3->begin(buffered,audioOut);drawPlayer();}
    else if(key=='+'||key=='='){volume=min(100,volume+5);drawPlayer();}
    else if(key=='-'||key=='_'){volume=max(0,volume-5);drawPlayer();}
    else if(key==0x1B||key==0x60||key=='~'||keys.del){stopPlayback();drawLibrary();}
    return;
  }
  if(key==';'||key==',')changeSelection(-1);
  else if(key=='.'||key=='/')changeSelection(1);
  else if(keys.enter&&fileCount)startTrack(files[selected]);
}
