#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <AudioOutput.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGenerator.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include "johnny_version.h"

namespace {
constexpr int CS=12,SCK_PIN=40,MISO_PIN=39,MOSI_PIN=14;
constexpr uint16_t BG=0x0000,PANEL=0x2800,RED=0xF800,WHITE=0xFFFF,GREY=0x9CF3,DIM=0x528A;
SPIClass sd(FSPI); bool sdOK=false;
enum Page:uint8_t{HOME,MUSIC,IMAGES,FILES,PLAYER,VIEWER,EQ}; Page page=HOME;
String names[64],paths[64],track=""; bool dirs[64]; int total=0,selected=0,offset=0,volume=72,preset=0;
String folder="/"; bool playing=false,paused=false;
alignas(4) uint8_t audioBuffer[4096];
AudioFileSourceSD* source=nullptr; AudioFileSourceBuffer* stream=nullptr; AudioGenerator* decoder=nullptr;
float lowState=0,midState=0;
const char* presets[]={"FLAT","BASS","VOCAL","TREBLE"};
const float lo[]={1.0,1.7,0.8,0.6},mi[]={1.0,0.9,1.6,0.9},hi[]={1.0,0.9,1.1,1.7};
class JohnnyOutput:public AudioOutput{
 m5::Speaker_Class* sp; int16_t pcm[2][384]{}; size_t used=0; uint8_t slot=0;
 bool send(){if(!used)return true;uint32_t t=millis();while(!sp->playRaw(pcm[slot],used,hertz,false,1,0,false)){if(millis()-t>120)return false;delay(1);}slot^=1;used=0;return true;}
public:
 explicit JohnnyOutput(m5::Speaker_Class* s):sp(s){}
 bool begin()override{return sp!=nullptr;}
 bool ConsumeSample(int16_t sample[2])override{
  if(used==384&&!send())return false;
  float x=((float)sample[0]+sample[1])*0.5f;lowState+=0.035f*(x-lowState);
  float high=x-midState;midState+=0.22f*(x-midState);float mid=midState-lowState;
  float y=(lowState*lo[preset]+mid*mi[preset]+high*hi[preset])*volume/100.0f;
  y=constrain(y,-32768.0f,32767.0f);pcm[slot][used++]=(int16_t)y;return true;
 }
 void flush()override{send();}
 bool stop()override{used=0;sp->stop();return true;}
};
JohnnyOutput* output=nullptr;
bool ext(const String& p,const char* e){String s=p;s.toLowerCase();return s.endsWith(e);}
bool audio(const String& p){return ext(p,".mp3")||ext(p,".wav");}
bool image(const String& p){return ext(p,".jpg")||ext(p,".jpeg")||ext(p,".bmp");}
void stopPlayer(){
 if(decoder){if(decoder->isRunning())decoder->stop();delete decoder;decoder=nullptr;}
 if(stream){delete stream;stream=nullptr;} if(source){source->close();delete source;source=nullptr;}
 if(output){output->stop();delete output;output=nullptr;} M5Cardputer.Speaker.stop();playing=false;paused=false;
}
void header(const char* title){auto& d=M5Cardputer.Display;d.fillScreen(BG);d.fillRect(0,0,240,21,0x1800);d.setTextSize(1);d.setTextColor(RED,0x1800);d.setCursor(8,6);d.print("JOHNNY");d.setTextColor(GREY,0x1800);d.setCursor(190,6);d.print("ALPHA");d.drawFastHLine(8,23,224,RED);d.setTextSize(2);d.setTextColor(WHITE,BG);d.setCursor(8,30);d.print(title);d.setTextSize(1);}
void footer(const char* s){auto& d=M5Cardputer.Display;d.drawFastHLine(8,117,224,DIM);d.setTextColor(GREY,BG);d.setCursor(8,124);d.print(s);}
void scan(const String& dir){total=0;selected=0;offset=0;folder=dir;if(!sdOK)return;File f=SD.open(dir);if(!f||!f.isDirectory()){if(f)f.close();return;}File e=f.openNextFile();while(e&&total<64){String raw=String(e.name());bool isd=e.isDirectory();e.close();int s=raw.lastIndexOf('/');String n=s>=0?raw.substring(s+1):raw;if(n.length()&&n[0]!='.'){names[total]=n;paths[total]=dir=="/"?String("/")+n:dir+"/"+n;dirs[total++]=isd;}e=f.openNextFile();}f.close();}
void homeDraw(){header("LIBRARY");auto& d=M5Cardputer.Display;const char* a[]={"MUSIC PLAYER","IMAGES","EQUALIZER","FILES"};for(int i=0;i<4;i++){int y=52+i*16;bool on=i==selected;d.fillRoundRect(8,y-2,224,14,3,on?PANEL:BG);d.setTextColor(on?RED:GREY,on?PANEL:BG);d.setCursor(16,y);d.printf("%c  %s",on?'>':' ',a[i]);}d.setTextColor(DIM,BG);d.setCursor(8,105);d.printf("SD %s",sdOK?"READY":"MISSING");footer("UP/DOWN ENTER");}
void listDraw(){header(page==MUSIC?"MUSIC":page==IMAGES?"IMAGES":"FILES");auto& d=M5Cardputer.Display;if(!sdOK){d.setTextColor(RED,BG);d.setCursor(12,65);d.print("NO MICROSD");footer("INSERT CARD");return;}int row=0;for(int i=offset;i<total&&row<6;i++,row++){int y=51+row*10;bool on=i==selected;d.fillRect(8,y-1,224,10,on?PANEL:BG);d.setTextColor(on?RED:(dirs[i]?GREY:WHITE),on?PANEL:BG);String n=names[i];if(n.length()>30)n=n.substring(0,27)+"...";d.setCursor(12,y);d.printf("%c %s%s",on?'>':' ',n.c_str(),dirs[i]?"/":"");}if(!total){d.setTextColor(GREY,BG);d.setCursor(12,70);d.print("EMPTY");}footer("UP/DOWN ENTER ESC");}
bool play(const String& p){stopPlayer();source=new AudioFileSourceSD();if(!source||!source->open(p.c_str())){stopPlayer();return false;}stream=new AudioFileSourceBuffer(source,audioBuffer,sizeof(audioBuffer));output=new JohnnyOutput(&M5Cardputer.Speaker);if(!stream||!output){stopPlayer();return false;}output->SetGain(1.0f);decoder=ext(p,".wav")?(AudioGenerator*)new AudioGeneratorWAV():(AudioGenerator*)new AudioGeneratorMP3();if(!decoder||!decoder->begin(stream,output)){stopPlayer();return false;}track=p.substring(p.lastIndexOf('/')+1);lowState=midState=0;playing=true;paused=false;page=PLAYER;return true;}
void playerDraw(){header("NOW PLAYING");auto& d=M5Cardputer.Display;d.fillRoundRect(10,51,220,48,5,PANEL);d.setTextColor(RED,BG);d.setCursor(20,60);d.print(paused?"PAUSED":"PLAYING");d.setTextColor(WHITE,BG);d.setCursor(20,78);String n=track;if(n.length()>28)n=n.substring(0,25)+"...";d.print(n);d.setTextColor(GREY,BG);d.setCursor(20,91);d.printf("VOL %d%% EQ %s",volume,presets[preset]);footer("SPACE PAUSE P STOP +/- VOL");}
void eqDraw(){header("EQUALIZER");auto& d=M5Cardputer.Display;const char* b[]={"LOW","MID","HIGH"};int h[]={int(lo[preset]*22),int(mi[preset]*22),int(hi[preset]*22)};for(int i=0;i<3;i++){int x=42+i*62;d.drawRect(x,55,28,42,DIM);d.fillRect(x+3,94-h[i],22,h[i],RED);d.setTextColor(GREY,BG);d.setCursor(x,101);d.print(b[i]);}d.setTextColor(WHITE,BG);d.setCursor(8,43);d.printf("PROFILE %s",presets[preset]);footer("LEFT/RIGHT CHANGE ESC BACK");}
void showImage(const String& p){stopPlayer();page=VIEWER;auto& d=M5Cardputer.Display;d.fillScreen(BG);bool ok=ext(p,".bmp")?d.drawBmpFile(SD,p.c_str(),0,0):d.drawJpgFile(SD,p.c_str(),0,0);if(!ok){d.setTextColor(RED,BG);d.setCursor(12,55);d.print("IMAGE ERROR");}d.fillRect(0,119,240,17,BG);d.setTextColor(WHITE,BG);d.setCursor(5,124);String n=p.substring(p.lastIndexOf('/')+1);if(n.length()>34)n=n.substring(0,31)+"...";d.print(n);}
void activate(){if(page==HOME){if(selected==0){page=MUSIC;scan("/");}else if(selected==1){page=IMAGES;scan("/");}else if(selected==2){page=EQ;eqDraw();return;}else{page=FILES;scan("/");}listDraw();return;}if(selected>=total)return;if(dirs[selected]){String p=paths[selected];scan(p);listDraw();return;}String p=paths[selected];if((page==MUSIC||page==FILES)&&audio(p)){if(play(p))playerDraw();}else if((page==IMAGES||page==FILES)&&image(p))showImage(p);}
void back(){if(page==PLAYER){stopPlayer();page=MUSIC;listDraw();}else if(page==VIEWER){page=IMAGES;listDraw();}else if(page==EQ){page=HOME;homeDraw();}else if(page==MUSIC||page==IMAGES||page==FILES){if(folder!="/"){int s=folder.lastIndexOf('/');scan(s<=0?"/":folder.substring(0,s));listDraw();}else{page=HOME;selected=0;homeDraw();}}else homeDraw();}
void move(int delta){int max=page==HOME?4:total;if(!max)return;selected=constrain(selected+delta,0,max-1);if(selected<offset)offset=selected;if(selected>=offset+6)offset=selected-5;}
}
void setup(){Serial.begin(115200);auto cfg=M5.config();M5Cardputer.begin(cfg,true);M5Cardputer.Display.setRotation(1);M5Cardputer.Speaker.begin();pinMode(38,OUTPUT);digitalWrite(38,HIGH);sd.begin(SCK_PIN,MISO_PIN,MOSI_PIN,CS);sdOK=SD.begin(CS,sd,16000000);homeDraw();Serial.printf("JOHNNY %s SD=%d\n",JOHNNY_VERSION,sdOK);}
void loop(){M5Cardputer.update();if(page==PLAYER&&playing&&!paused&&decoder&&!decoder->loop()){stopPlayer();page=MUSIC;listDraw();}if(!M5Cardputer.Keyboard.isChange()||!M5Cardputer.Keyboard.isPressed()){delay(2);return;}auto k=M5Cardputer.Keyboard.keysState();char c=k.word.size()?k.word[0]:0;if(c==0x1B||c==0x60||c=='~'||k.del){back();return;}if(page==EQ){if(c==','||c=='-')preset=(preset+3)%4;else if(c=='.'||c=='+'||c=='/')preset=(preset+1)%4;eqDraw();return;}if(page==PLAYER){if(k.space||c==' '){paused=!paused;playerDraw();}else if(c=='p'||c=='P'){stopPlayer();page=MUSIC;listDraw();}else if(c=='-'||c=='_'){volume=max(0,volume-5);playerDraw();}else if(c=='='||c=='+'){volume=min(100,volume+5);playerDraw();}return;}if(page==VIEWER){back();return;}if(c==';'||c==',')move(-1);else if(c=='.'||c=='/')move(1);else if(k.enter){activate();return;}if(page==HOME)homeDraw();else listDraw();}
