#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <AudioOutput.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <WiFi.h>
#include <time.h>
#include <math.h>
#include "johnny_version.h"
namespace {
constexpr int CS=12,SCK_PIN=40,MISO_PIN=39,MOSI_PIN=14;
constexpr uint16_t BG=0x0000,PANEL=0x2104,LIGHT=0xFC8E,MUTED=0x8410,EDGE=0x5000;uint16_t JOHNNY_RED=0xF124;
SPIClass sd(FSPI);bool sdOK=false;
enum Page:uint8_t{HOME,LIST,PLAYER,SYSTEM,WIFI_SCAN,SYNTH,TUNER,RECORDER,GAME,SETTINGS};Page page=HOME;
bool musicOnly=true,playing=false,paused=false;bool micActive=false;uint8_t brightness=180;bool alternateTheme=false;uint32_t lastGameTick=0;int snakeX[80],snakeY[80],snakeLen=4,dirX=1,dirY=0,foodX=12,foodY=6;bool gameOver=false;static int16_t recordSamples[32000];
String folder="/",track="";String files[64],paths[64];bool dirs[64];int total=0,indexFile=0,offset=0,volume=70;
AudioFileSourceSD* source=nullptr;AudioFileSourceBuffer* buffer=nullptr;AudioGeneratorMP3* mp3=nullptr;alignas(4) uint8_t decodeBuffer[4096];
class JohnnyOutput:public AudioOutput{
 m5::Speaker_Class* speaker;int16_t pcm[3][512]{};size_t used=0;uint8_t slot=0;
 bool send(){if(!used)return true;uint32_t t=millis();while(!speaker->playRaw(pcm[slot],used,hertz,false,1,0,false)){if(millis()-t>150)return false;delay(1);}slot=(slot+1)%3;used=0;return true;}
public:
 explicit JohnnyOutput(m5::Speaker_Class* s):speaker(s){}
 bool begin()override{return speaker!=nullptr;}
 bool ConsumeSample(int16_t sample[2])override{if(used==512&&!send())return false;int32_t v=((int32_t)sample[0]+sample[1])/2;v=(int32_t)(v*(float)volume/100.0f);v=constrain(v,-32768,32767);pcm[slot][used++]=(int16_t)v;return true;}
 void flush()override{send();}
 bool stop()override{used=0;speaker->stop();return true;}
};
JohnnyOutput* out=nullptr;
void closePlayer(){if(mp3){if(mp3->isRunning())mp3->stop();delete mp3;mp3=nullptr;}if(buffer){delete buffer;buffer=nullptr;}if(source){source->close();delete source;source=nullptr;}if(out){out->stop();delete out;out=nullptr;}M5Cardputer.Speaker.stop();playing=false;paused=false;}
void header(const char* title){auto& d=M5Cardputer.Display;d.fillScreen(BG);d.fillRect(0,0,240,20,0x180B);d.setTextSize(1);d.setTextColor(JOHNNY_RED,BG);d.setCursor(8,6);d.print("JOHNNY");d.setTextColor(MUTED,BG);d.setCursor(190,6);d.print("ADV");d.drawFastHLine(8,22,224,EDGE);d.setTextSize(2);d.setTextColor(WHITE,BG);d.setCursor(8,30);d.print(title);d.setTextSize(1);}
void footer(const char* s){auto& d=M5Cardputer.Display;d.drawFastHLine(8,117,224,EDGE);d.setTextColor(MUTED,BG);d.setCursor(8,123);d.print(s);}
void homeDraw(){
 header("JOHNNY OS");auto& d=M5Cardputer.Display;
 const char* a[]={"MUSIC PLAYER","SYNTH","TUNER","RECORDER","WI-FI SCAN","SD FILES","SNAKE","SETTINGS","SYSTEM"};
 int first=indexFile>3?indexFile-3:0;
 for(int row=0;row<4&&first+row<9;row++){int i=first+row,y=52+row*16;bool on=i==indexFile;d.fillRoundRect(8,y-2,224,14,3,on?PANEL:BG);d.setTextColor(on?JOHNNY_RED:LIGHT,on?PANEL:BG);d.setCursor(16,y);d.printf("%c %s",on?'>':' ',a[i]);}
 footer("UP/DOWN ENTER");
}
void scanDir(const String& path,uint8_t depth=0){if(!sdOK||depth>2||total>=64)return;File d=SD.open(path,FILE_READ);if(!d||!d.isDirectory()){if(d)d.close();return;}File f=d.openNextFile();while(f&&total<64){String raw=String(f.name());bool isdir=f.isDirectory();f.close();int slash=raw.lastIndexOf('/');String n=slash>=0?raw.substring(slash+1):raw;if(n.length()&&n[0]!='.'&&n!="LOST.DIR"){String full=path=="/"?String("/")+n:path+"/"+n;if(musicOnly){if(isdir)scanDir(full,depth+1);else{String lo=n;lo.toLowerCase();if(lo.endsWith(".mp3")){files[total]=n;paths[total]=full;dirs[total++]=false;}}}else{files[total]=n;paths[total]=full;dirs[total++]=isdir;}}f=d.openNextFile();}d.close();}
void scan(bool music){musicOnly=music;total=0;indexFile=0;offset=0;scanDir(music?"/":folder);}
void listDraw(){header(musicOnly?"MUSIC":"FILES");auto& d=M5Cardputer.Display;if(!sdOK){d.setTextColor(JOHNNY_RED,BG);d.setCursor(12,65);d.print("NO MICROSD");footer("CHECK CARD");return;}int row=0;for(int i=offset;i<total&&row<5;i++,row++){int y=musicOnly?54+row*12:56+row*11;bool on=i==indexFile;d.fillRect(8,y-1,224,11,on?PANEL:BG);d.setTextColor(on?JOHNNY_RED:(dirs[i]?LIGHT:WHITE),on?PANEL:BG);d.setCursor(12,y);String n=files[i];if(n.length()>29)n=n.substring(0,26)+"...";d.printf("%c %s%s",on?'>':' ',n.c_str(),dirs[i]?"/":"");}if(total==0){d.setTextColor(MUTED,BG);d.setCursor(12,70);d.print("EMPTY");}footer("UP/DOWN ENTER BACK");}
bool playFile(const String& p){closePlayer();source=new AudioFileSourceSD();if(!source||!source->open(p.c_str())){closePlayer();return false;}buffer=new AudioFileSourceBuffer(source,decodeBuffer,sizeof(decodeBuffer));out=new JohnnyOutput(&M5Cardputer.Speaker);if(!buffer||!out){closePlayer();return false;}out->SetGain(0.7f);mp3=new AudioGeneratorMP3();if(!mp3||!mp3->begin(buffer,out)){closePlayer();return false;}track=p.substring(p.lastIndexOf('/')+1);playing=true;paused=false;M5Cardputer.Speaker.setVolume(205);return true;}
void playerDraw(){header("MUSIC PLAYER");auto& d=M5Cardputer.Display;d.fillRoundRect(10,51,220,48,5,PANEL);d.setTextColor(JOHNNY_RED,BG);d.setCursor(20,60);d.print(paused?"PAUSED":playing?"PLAYING":"STOPPED");d.setTextColor(WHITE,BG);d.setCursor(20,79);String n=track;if(n.length()>27)n=n.substring(0,24)+"...";d.print(n);d.setTextColor(LIGHT,BG);d.setCursor(20,91);d.printf("VOLUME %d%%",volume);footer("SPACE PAUSE  +/- VOL  P STOP");}
void openFile(){if(indexFile<0||indexFile>=total)return;if(musicOnly){if(playFile(paths[indexFile])){page=PLAYER;playerDraw();}}else if(dirs[indexFile]){folder=paths[indexFile];scan(false);listDraw();}else{String p=paths[indexFile];p.toLowerCase();if(p.endsWith(".mp3")&&playFile(paths[indexFile])){page=PLAYER;playerDraw();}}}
void back(){if(page==PLAYER){closePlayer();page=LIST;listDraw();}else if(page==LIST){if(!musicOnly&&folder!="/"){int s=folder.lastIndexOf('/');folder=s<=0?"/":folder.substring(0,s);scan(false);listDraw();}else{page=HOME;indexFile=0;homeDraw();}}else if(page==SYSTEM){page=HOME;indexFile=8;homeDraw();}else{page=HOME;homeDraw();}}
void startList(bool music){folder="/";scan(music);page=LIST;listDraw();}
void systemDraw(){header("SYSTEM");auto& d=M5Cardputer.Display;d.setTextColor(LIGHT,BG);d.setCursor(10,53);d.print("JOHNNY OS");d.setTextColor(WHITE,BG);d.setCursor(10,71);d.printf("VERSION %s",JOHNNY_VERSION);d.setTextColor(MUTED,BG);d.setCursor(10,91);d.printf("SD %s",sdOK?"READY":"MISSING");d.setCursor(10,105);d.printf("HEAP %lu KB",ESP.getFreeHeap()/1024UL);footer("BACK");}
}
void settingsDraw(){header("SETTINGS");auto& d=M5Cardputer.Display;d.setTextColor(LIGHT,BG);d.setCursor(12,55);d.printf("BRIGHTNESS %u",brightness);d.setCursor(12,73);d.printf("VOLUME %d",volume);d.setCursor(12,91);d.printf("ACCENT %s",alternateTheme?"AMBER":"CRIMSON");footer("; . BRIGHT  +/- VOL  T THEME");}
void gameDraw(){header("SNAKE");auto& d=M5Cardputer.Display;d.drawRect(8,48,224,80,EDGE);for(int i=0;i<snakeLen;i++)d.fillRect(12+snakeX[i]*7,51+snakeY[i]*6,5,4,i==0?JOHNNY_RED:LIGHT);d.fillRect(12+foodX*7,51+foodY*6,5,4,0xFD20);if(gameOver){d.setTextColor(JOHNNY_RED,BG);d.setCursor(70,85);d.print("GAME OVER");}footer("W A S D MOVE  BACK");}
void micRestore(){if(micActive){while(M5Cardputer.Mic.isRecording())delay(1);M5Cardputer.Mic.end();micActive=false;}M5Cardputer.Speaker.begin();M5Cardputer.Speaker.setVolume((uint8_t)(volume*255/100));}
void tunerUpdate(){static int16_t samples[512];if(!micActive)return;if(!M5Cardputer.Mic.record(samples,512,16000))return;int crossings=0;int32_t mean=0;for(int i=0;i<512;i++)mean+=samples[i];mean/=512;for(int i=1;i<512;i++)if((samples[i-1]-mean)<=0&&(samples[i]-mean)>0)crossings++;float hz=(float)crossings*16000.0f/512.0f;header("TUNER");auto& d=M5Cardputer.Display;d.setTextColor(WHITE,BG);d.setTextSize(3);d.setCursor(24,58);if(hz>35&&hz<1200){int midi=(int)roundf(69.0f+12.0f*log2f(hz/440.0f));const char* names[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};d.printf("%s%d",names[(midi%12+12)%12],midi/12-1);d.setTextSize(1);d.setCursor(25,99);d.printf("%.1f Hz",hz);}else{d.print("--");d.setTextSize(1);d.setCursor(25,99);d.print("PLAY A NOTE");}footer("MIC INPUT / BACK");}
bool saveRecording(){if(!sdOK)return false;char name[28];uint32_t id=millis();snprintf(name,sizeof(name),"/REC%lu.WAV",(unsigned long)id);File f=SD.open(name,FILE_WRITE);if(!f)return false;uint32_t dataBytes=sizeof(recordSamples);f.write((const uint8_t*)"RIFF",4);uint32_t riffSize=36+dataBytes;f.write((uint8_t*)&riffSize,4);f.write((const uint8_t*)"WAVEfmt ",8);uint32_t fmtSize=16;f.write((uint8_t*)&fmtSize,4);uint16_t pcm=1,channels=1,bits=16,align=2;uint32_t rate=16000,byteRate=32000;f.write((uint8_t*)&pcm,2);f.write((uint8_t*)&channels,2);f.write((uint8_t*)&rate,4);f.write((uint8_t*)&byteRate,4);f.write((uint8_t*)&align,2);f.write((uint8_t*)&bits,2);f.write((const uint8_t*)"data",4);f.write((uint8_t*)&dataBytes,4);size_t wrote=f.write((uint8_t*)recordSamples,dataBytes);f.flush();f.close();return wrote==dataBytes;}
void recordAudio(){if(!sdOK){header("NO MICROSD");footer("INSERT CARD");return;}M5Cardputer.Speaker.end();micActive=M5Cardputer.Mic.begin();if(!micActive){header("MIC ERROR");M5Cardputer.Speaker.begin();footer("BACK");return;}header("RECORDING");auto& d=M5Cardputer.Display;d.setTextColor(JOHNNY_RED,BG);d.setCursor(22,65);d.print("REC / 2 SECONDS");for(int i=0;i<32000;i+=512){if(!M5Cardputer.Mic.record(&recordSamples[i],512,16000))break;M5Cardputer.update();}M5Cardputer.Mic.end();micActive=false;M5Cardputer.Speaker.begin();bool ok=saveRecording();header(ok?"SAVED WAV":"SAVE FAILED");footer("BACK");}
void gameStep(){if(millis()-lastGameTick<145)return;lastGameTick=millis();int nx=snakeX[0]+dirX,ny=snakeY[0]+dirY;if(nx<0||nx>=30||ny<0||ny>=12){gameOver=true;gameDraw();return;}for(int i=0;i<snakeLen;i++)if(snakeX[i]==nx&&snakeY[i]==ny){gameOver=true;gameDraw();return;}for(int i=snakeLen-1;i>0;i--){snakeX[i]=snakeX[i-1];snakeY[i]=snakeY[i-1];}snakeX[0]=nx;snakeY[0]=ny;if(nx==foodX&&ny==foodY&&snakeLen<79){snakeLen++;foodX=random(1,28);foodY=random(1,11);}gameDraw();}
void bootSplash(){
 auto& d=M5Cardputer.Display;
 d.fillScreen(BG);
 d.setTextDatum(middle_center);
 d.setTextSize(3);
 d.setTextColor(WHITE,BG);
 d.drawString("JOHNNY",120,57);
 d.setTextSize(1);
 d.setTextColor(JOHNNY_RED,BG);
 d.drawString("WAKE UP SAMURAI",120,86);
 d.drawFastHLine(38,101,164,EDGE);
 const uint32_t started=millis();
 uint8_t frame=0;
 while(millis()-started<5000){
  if((frame%3)==0){const int x=random(42,198);const int y=random(43,76);d.drawFastVLine(x,y,random(2,9),JOHNNY_RED);}
  if((frame%7)==0){const int y=random(43,78);d.drawFastHLine(random(45,105),y,random(12,74),EDGE);}
  M5Cardputer.update();delay(45);frame++;
 }
 d.setTextDatum(top_left);d.fillScreen(BG);
}
void setup(){Serial.begin(115200);auto cfg=M5.config();M5Cardputer.begin(cfg,true);M5Cardputer.Display.setRotation(1);M5Cardputer.Speaker.begin();M5Cardputer.Speaker.setVolume(190);pinMode(38,OUTPUT);digitalWrite(38,HIGH);sd.begin(SCK_PIN,MISO_PIN,MOSI_PIN,CS);sdOK=SD.begin(CS,sd,16000000);if(sdOK){File cfgFile=SD.open("/JOHNNY.CFG",FILE_READ);if(cfgFile){int b=180,v=70,t=0;String cfgText=cfgFile.readString();cfgFile.close();if(sscanf(cfgText.c_str(),"%d,%d,%d",&b,&v,&t)==3){brightness=(uint8_t)constrain(b,20,255);volume=constrain(v,0,100);alternateTheme=t!=0;JOHNNY_RED=alternateTheme?0xFD20:0xF124;}}}M5Cardputer.Display.setBrightness(brightness);bootSplash();homeDraw();Serial.printf("JOHNNY %s SD=%d\n",JOHNNY_VERSION,sdOK);}
void loop(){
 M5Cardputer.update();
 if(page==PLAYER&&playing&&!paused&&mp3&&!mp3->loop()){closePlayer();page=LIST;listDraw();}
 if(page==TUNER){if(M5Cardputer.Keyboard.isChange()&&M5Cardputer.Keyboard.isPressed()){auto k=M5Cardputer.Keyboard.keysState();if(k.del||k.word.size()&&k.word[0]==0x1B){micRestore();page=HOME;homeDraw();return;}}tunerUpdate();return;}
 if(page==GAME){if(M5Cardputer.Keyboard.isChange()&&M5Cardputer.Keyboard.isPressed()){auto k=M5Cardputer.Keyboard.keysState();char c=k.word.size()?k.word[0]:0;if(c==0x1B||c==0x60||k.del){page=HOME;homeDraw();return;}if(!gameOver){if(c=='w'&&dirY!=1){dirX=0;dirY=-1;}else if(c=='s'&&dirY!=-1){dirX=0;dirY=1;}else if(c=='a'&&dirX!=1){dirX=-1;dirY=0;}else if(c=='d'&&dirX!=-1){dirX=1;dirY=0;}}else if(k.enter){snakeLen=4;for(int i=0;i<snakeLen;i++){snakeX[i]=8-i;snakeY[i]=6;}dirX=1;dirY=0;gameOver=false;}}gameStep();return;}
 if(!M5Cardputer.Keyboard.isChange()||!M5Cardputer.Keyboard.isPressed()){delay(2);return;}
 auto k=M5Cardputer.Keyboard.keysState();char c=k.word.size()?k.word[0]:0;
 if(c==0x1B||c==0x60||c=='~'||k.del){if(page==PLAYER){back();return;}if(page==TUNER){micRestore();}if(page==SYNTH){M5Cardputer.Speaker.stop();}page=HOME;homeDraw();return;}
 if(page==HOME){if(c==';'||c==',')indexFile=(indexFile+8)%9;else if(c=='.'||c=='/')indexFile=(indexFile+1)%9;else if(k.enter){if(indexFile==0)startList(true);else if(indexFile==1){page=SYNTH;header("SYNTH");footer("A S D F G H J / BACK");}else if(indexFile==2){page=TUNER;M5Cardputer.Speaker.end();micActive=M5Cardputer.Mic.begin();header("TUNER");footer("PLAY A NOTE / BACK");}else if(indexFile==3){page=RECORDER;header("RECORDER");footer("R RECORD  BACK");}else if(indexFile==4){page=WIFI_SCAN;header("WI-FI SCAN");footer("SCANNING...");WiFi.mode(WIFI_STA);int n=WiFi.scanNetworks(false,true);header("WI-FI NETWORKS");for(int i=0;i<n&&i<6;i++){M5Cardputer.Display.setCursor(8,51+i*10);M5Cardputer.Display.setTextColor(WHITE,BG);String name=WiFi.SSID(i);if(name.length()>19)name=name.substring(0,19);M5Cardputer.Display.printf("%s %d",name.c_str(),WiFi.RSSI(i));}footer("BACK");WiFi.scanDelete();}else if(indexFile==5)startList(false);else if(indexFile==6){page=GAME;snakeLen=4;for(int i=0;i<snakeLen;i++){snakeX[i]=8-i;snakeY[i]=6;}dirX=1;dirY=0;gameOver=false;foodX=random(1,28);foodY=random(1,11);lastGameTick=millis();gameDraw();}else if(indexFile==7){page=SETTINGS;settingsDraw();}else{page=SYSTEM;systemDraw();}}else homeDraw();}
 else if(page==LIST){if(c==';'||c==','){if(indexFile>0)indexFile--;if(indexFile<offset)offset=indexFile;}else if(c=='.'||c=='/'){if(indexFile<total-1)indexFile++;if(indexFile>=offset+5)offset=indexFile-4;}else if(k.enter)openFile();listDraw();}
 else if(page==PLAYER){if(k.space||c==' '){paused=!paused;playerDraw();}else if(c=='p'||c=='P'){closePlayer();page=LIST;listDraw();}else if(c=='-'||c=='_'){volume=max(0,volume-5);if(out)out->SetGain((float)volume/100.0f);playerDraw();}else if(c=='='||c=='+'){volume=min(100,volume+5);if(out)out->SetGain((float)volume/100.0f);playerDraw();}}
 else if(page==SYNTH){const char* keys="asdfghjkl";const int notes[]={262,294,330,349,392,440,494,523,587};for(int i=0;i<9;i++)if(c==keys[i]){M5Cardputer.Speaker.tone(notes[i],160);break;}footer("A S D F G H J K L / BACK");}
 else if(page==RECORDER){if(c=='r'||c=='R')recordAudio();else footer("R RECORD  BACK");}
 else if(page==SETTINGS){if(c==';'||c==','){brightness=brightness>40?brightness-20:20;M5Cardputer.Display.setBrightness(brightness);}else if(c=='.'||c=='/'){volume=min(100,volume+5);M5Cardputer.Speaker.setVolume((uint8_t)(volume*255/100));}else if(c=='-'||c=='_'){volume=max(0,volume-5);M5Cardputer.Speaker.setVolume((uint8_t)(volume*255/100));}else if(c=='t'||c=='T'){alternateTheme=!alternateTheme;JOHNNY_RED=alternateTheme?0xFD20:0xF124;}else if(k.enter){if(sdOK){SD.remove("/JOHNNY.CFG");File f=SD.open("/JOHNNY.CFG",FILE_WRITE);if(f){f.printf("%u,%d,%u",brightness,volume,alternateTheme?1:0);f.close();}}}settingsDraw();}
 else if(page==SYSTEM)systemDraw();
}