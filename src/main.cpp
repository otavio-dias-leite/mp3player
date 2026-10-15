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
constexpr int CS=12,SCK_PIN=40,MISO_PIN=39,MOSI_PIN=14;
constexpr uint16_t BG=0x0000,PANEL=0x2104,PINK=0xF124,LIGHT=0xFC8E,MUTED=0x8410,EDGE=0x5000;
SPIClass sd(FSPI);bool sdOK=false;
enum Page:uint8_t{HOME,LIST,PLAYER,SYSTEM};Page page=HOME;
bool musicOnly=true,playing=false,paused=false;
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
void header(const char* title){auto& d=M5Cardputer.Display;d.fillScreen(BG);d.fillRect(0,0,240,20,0x180B);d.setTextSize(1);d.setTextColor(PINK,BG);d.setCursor(8,6);d.print("JOHNNY");d.setTextColor(MUTED,BG);d.setCursor(190,6);d.print("ADV");d.drawFastHLine(8,22,224,EDGE);d.setTextSize(2);d.setTextColor(WHITE,BG);d.setCursor(8,30);d.print(title);d.setTextSize(1);}
void footer(const char* s){auto& d=M5Cardputer.Display;d.drawFastHLine(8,117,224,EDGE);d.setTextColor(MUTED,BG);d.setCursor(8,123);d.print(s);}
void homeDraw(){header("HOME");auto& d=M5Cardputer.Display;const char* a[]={"MUSIC PLAYER","SD FILES","SYSTEM"};for(int i=0;i<3;i++){int y=54+i*21;bool on=i==indexFile;d.fillRoundRect(8,y-3,224,18,4,on?PANEL:BG);d.setTextColor(on?PINK:LIGHT,on?PANEL:BG);d.setCursor(16,y+1);d.printf("%c %s",on?'>':' ',a[i]);}footer("UP/DOWN ENTER");}
void scanDir(const String& path,uint8_t depth=0){if(!sdOK||depth>2||total>=64)return;File d=SD.open(path,FILE_READ);if(!d||!d.isDirectory()){if(d)d.close();return;}File f=d.openNextFile();while(f&&total<64){String raw=String(f.name());bool isdir=f.isDirectory();f.close();int slash=raw.lastIndexOf('/');String n=slash>=0?raw.substring(slash+1):raw;if(n.length()&&n[0]!='.'&&n!="LOST.DIR"){String full=path=="/"?String("/")+n:path+"/"+n;if(musicOnly){if(isdir)scanDir(full,depth+1);else{String lo=n;lo.toLowerCase();if(lo.endsWith(".mp3")){files[total]=n;paths[total]=full;dirs[total++]=false;}}}else{files[total]=n;paths[total]=full;dirs[total++]=isdir;}}f=d.openNextFile();}d.close();}
void scan(bool music){musicOnly=music;total=0;indexFile=0;offset=0;scanDir(music?"/":folder);}
void listDraw(){header(musicOnly?"MUSIC":"FILES");auto& d=M5Cardputer.Display;if(!sdOK){d.setTextColor(PINK,BG);d.setCursor(12,65);d.print("NO MICROSD");footer("CHECK CARD");return;}int row=0;for(int i=offset;i<total&&row<5;i++,row++){int y=musicOnly?54+row*12:56+row*11;bool on=i==indexFile;d.fillRect(8,y-1,224,11,on?PANEL:BG);d.setTextColor(on?PINK:(dirs[i]?LIGHT:WHITE),on?PANEL:BG);d.setCursor(12,y);String n=files[i];if(n.length()>29)n=n.substring(0,26)+"...";d.printf("%c %s%s",on?'>':' ',n.c_str(),dirs[i]?"/":"");}if(total==0){d.setTextColor(MUTED,BG);d.setCursor(12,70);d.print("EMPTY");}footer("UP/DOWN ENTER BACK");}
bool playFile(const String& p){closePlayer();source=new AudioFileSourceSD();if(!source||!source->open(p.c_str())){closePlayer();return false;}buffer=new AudioFileSourceBuffer(source,decodeBuffer,sizeof(decodeBuffer));out=new JohnnyOutput(&M5Cardputer.Speaker);if(!buffer||!out){closePlayer();return false;}out->SetGain(0.7f);mp3=new AudioGeneratorMP3();if(!mp3||!mp3->begin(buffer,out)){closePlayer();return false;}track=p.substring(p.lastIndexOf('/')+1);playing=true;paused=false;M5Cardputer.Speaker.setVolume(205);return true;}
void playerDraw(){header("MUSIC PLAYER");auto& d=M5Cardputer.Display;d.fillRoundRect(10,51,220,48,5,PANEL);d.setTextColor(PINK,BG);d.setCursor(20,60);d.print(paused?"PAUSED":playing?"PLAYING":"STOPPED");d.setTextColor(WHITE,BG);d.setCursor(20,79);String n=track;if(n.length()>27)n=n.substring(0,24)+"...";d.print(n);d.setTextColor(LIGHT,BG);d.setCursor(20,91);d.printf("VOLUME %d%%",volume);footer("SPACE PAUSE  +/- VOL  P STOP");}
void openFile(){if(indexFile<0||indexFile>=total)return;if(musicOnly){if(playFile(paths[indexFile])){page=PLAYER;playerDraw();}}else if(dirs[indexFile]){folder=paths[indexFile];scan(false);listDraw();}else{String p=paths[indexFile];p.toLowerCase();if(p.endsWith(".mp3")&&playFile(paths[indexFile])){page=PLAYER;playerDraw();}}}
void back(){if(page==PLAYER){closePlayer();page=LIST;listDraw();}else if(page==LIST){if(!musicOnly&&folder!="/"){int s=folder.lastIndexOf('/');folder=s<=0?"/":folder.substring(0,s);scan(false);listDraw();}else{page=HOME;indexFile=0;homeDraw();}}else if(page==SYSTEM){page=HOME;indexFile=2;homeDraw();}else{page=HOME;homeDraw();}}
void startList(bool music){folder="/";scan(music);page=LIST;listDraw();}
void systemDraw(){header("SYSTEM");auto& d=M5Cardputer.Display;d.setTextColor(LIGHT,BG);d.setCursor(10,53);d.print("JOHNNY LITE");d.setTextColor(WHITE,BG);d.setCursor(10,71);d.printf("VERSION %s",JOHNNY_VERSION);d.setTextColor(MUTED,BG);d.setCursor(10,91);d.printf("SD %s",sdOK?"READY":"MISSING");d.setCursor(10,105);d.printf("HEAP %lu KB",ESP.getFreeHeap()/1024UL);footer("BACK");}
}
void bootSplash(){
 auto& d=M5Cardputer.Display;
 d.fillScreen(BG);
 d.setTextDatum(middle_center);
 d.setTextSize(3);
 d.setTextColor(WHITE,BG);
 d.drawString("JOHNNY",120,57);
 d.setTextSize(1);
 d.setTextColor(PINK,BG);
 d.drawString("WAKE UP SAMURAI",120,86);
 d.drawFastHLine(38,101,164,EDGE);
 const uint32_t started=millis();
 uint8_t frame=0;
 while(millis()-started<5000){
  if((frame%3)==0){const int x=random(42,198);const int y=random(43,76);d.drawFastVLine(x,y,random(2,9),PINK);}
  if((frame%7)==0){const int y=random(43,78);d.drawFastHLine(random(45,105),y,random(12,74),EDGE);}
  M5Cardputer.update();delay(45);frame++;
 }
 d.setTextDatum(top_left);d.fillScreen(BG);
}
void setup(){Serial.begin(115200);auto cfg=M5.config();M5Cardputer.begin(cfg,true);M5Cardputer.Display.setRotation(1);M5Cardputer.Speaker.begin();M5Cardputer.Speaker.setVolume(190);pinMode(38,OUTPUT);digitalWrite(38,HIGH);sd.begin(SCK_PIN,MISO_PIN,MOSI_PIN,CS);sdOK=SD.begin(CS,sd,16000000);bootSplash();homeDraw();Serial.printf("JOHNNY %s SD=%d\n",JOHNNY_VERSION,sdOK);}
void loop(){M5Cardputer.update();if(page==PLAYER&&playing&&!paused&&mp3&&!mp3->loop()){closePlayer();page=LIST;listDraw();}if(!M5Cardputer.Keyboard.isChange()||!M5Cardputer.Keyboard.isPressed()){delay(2);return;}auto k=M5Cardputer.Keyboard.keysState();char c=k.word.size()?k.word[0]:0;if(c==0x1B||c==0x60||c=='~'||k.del){back();return;}if(page==HOME){if(c==';'||c==',')indexFile=(indexFile+2)%3;else if(c=='.'||c=='/')indexFile=(indexFile+1)%3;else if(k.enter){if(indexFile==0)startList(true);else if(indexFile==1)startList(false);else{page=SYSTEM;systemDraw();}}else homeDraw();}else if(page==LIST){if(c==';'||c==','){if(indexFile>0)indexFile--;if(indexFile<offset)offset=indexFile;}else if(c=='.'||c=='/'){if(indexFile<total-1)indexFile++;if(indexFile>=offset+5)offset=indexFile-4;}else if(k.enter)openFile();listDraw();}else if(page==PLAYER){if(k.space||c==' '){paused=!paused;playerDraw();}else if(c=='p'||c=='P'){closePlayer();page=LIST;listDraw();}else if(c=='-'||c=='_'){volume=max(0,volume-5);if(out)out->SetGain((float)volume/100.0f);playerDraw();}else if(c=='='||c=='+'){volume=min(100,volume+5);if(out)out->SetGain((float)volume/100.0f);playerDraw();}}else if(page==SYSTEM)systemDraw();}
