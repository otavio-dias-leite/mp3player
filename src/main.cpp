#include <Arduino.h>
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>

namespace {
constexpr int SD_CS = 12, SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14;
constexpr uint16_t BG = 0x080B, PANEL = 0x190F, PURPLE = 0x895F, PINK = 0xF19F, CYAN = 0x4EFF, MUTED = 0x9CB0;
enum Screen : uint8_t { HOME, PIANO, TUNER, FILES, MUSIC, GALLERY, ABOUT };
Screen screen = HOME;
SPIClass sdBus(FSPI);
bool sdReady = false;
String currentPath = "/";
String entries[48];
bool entryIsDir[48];
int entryCount = 0, selected = 0, scroll = 0, tunerInstrument = 0;
String selectedFile;
const char* apps[] = {"POCKET PIANO", "CHROMATIC TUNER", "MUSIC LIBRARY", "SD EXPLORER", "PHOTO VIEWER", "SYSTEM"};
constexpr int APP_COUNT = 6;
const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
const float guitarHz[] = {82.41f,110.00f,146.83f,196.00f,246.94f,329.63f};
const char* guitarNames[] = {"E2","A2","D3","G3","B3","E4"};
const float ukeHz[] = {196.00f,261.63f,329.63f,392.00f};
const char* ukeNames[] = {"G4","C4","E4","A4"};
}

void header(const char* title) {
  auto &d = M5Cardputer.Display;
  d.fillScreen(BG);
  d.setTextSize(1);
  d.setTextColor(CYAN, BG); d.setCursor(7,5); d.print("J / JOHNNY");
  d.setTextColor(MUTED, BG); d.setCursor(178,5); d.print("ADV");
  d.drawFastHLine(7,18,226,PURPLE);
  d.setTextSize(2); d.setTextColor(WHITE,BG); d.setCursor(8,25); d.print(title);
  d.setTextSize(1);
}
void footer(const char* s="ESC  BACK") {
  auto &d=M5Cardputer.Display;
  d.drawFastHLine(7,117,226,0x392B);
  d.setTextColor(MUTED,BG); d.setCursor(8,124); d.print(s);
}
void drawHome() {
  header("APPLICATIONS");
  auto &d=M5Cardputer.Display;
  for(int i=0;i<APP_COUNT;i++){
    int y=48+i*11; bool on=i==selected;
    d.fillRect(8,y-1,224,10,on?PANEL:BG);
    if(on)d.fillRect(8,y-1,2,10,PINK);
    d.setTextColor(on?WHITE:MUTED,on?PANEL:BG);
    d.setCursor(15,y); d.printf("%02d  %s",i+1,apps[i]);
  }
  footer("W/S OR ARROWS  MOVE     ENTER  OPEN");
}
void splash() {
  auto &d=M5Cardputer.Display;
  d.fillScreen(BG); d.setTextSize(3); d.setTextColor(WHITE,BG);
  d.setCursor(36,48); d.print("JOHNNY"); delay(260);
  for(int i=0;i<8;i++){
    int y=random(36,98);
    d.fillRect(23,y,random(40,194),random(1,4),i%2?PINK:CYAN);
    d.setTextColor(i%2?PINK:CYAN,BG);
    d.setCursor(35+random(-4,5),47+random(-2,3)); d.print("JOHNNY");
    delay(42);
  }
  d.fillScreen(BG); d.setTextColor(WHITE,BG); d.setCursor(36,48); d.print("JOHNNY");
  d.setTextSize(1); d.setTextColor(PURPLE,BG); d.setCursor(65,83); d.print("PERSONAL DECK / 01");
  delay(550);
}
void pianoScreen() {
  header("POCKET PIANO");
  auto &d=M5Cardputer.Display;
  d.setTextColor(MUTED,BG); d.setCursor(8,42); d.print("Q W E R T Y U  /  WHITE KEYS");
  const char* labels[]={"C","D","E","F","G","A","B"};
  for(int i=0;i<7;i++){
    int x=8+i*32;
    d.fillRoundRect(x,58,29,48,2,0xDEFB);
    d.setTextColor(BG,0xDEFB); d.setCursor(x+10,88); d.print(labels[i]);
  }
  d.setTextColor(PINK,BG); d.setCursor(8,108); d.print("OCTAVE 4   |   SPEAKER SYNTH");
  footer("Q-U  PLAY     A-L  SHARPS     ESC  BACK");
}
void playNote(char c) {
  const char white[]={'q','w','e','r','t','y','u'};
  const int midi[]={60,62,64,65,67,69,71};
  const char black[]={'2','3','5','6','7'};
  const int blackMidi[]={61,63,66,68,70};
  for(int i=0;i<7;i++) if(c==white[i] || c==white[i]-32) {
    M5Cardputer.Speaker.tone((uint32_t)(440.0f*powf(2.0f,(midi[i]-69)/12.0f)),180); return;
  }
  for(int i=0;i<5;i++) if(c==black[i]) {
    M5Cardputer.Speaker.tone((uint32_t)(440.0f*powf(2.0f,(blackMidi[i]-69)/12.0f)),180); return;
  }
}
void tunerScreen() {
  header("CHROMATIC TUNER");
  auto &d=M5Cardputer.Display;
  d.fillRoundRect(8,45,106,23,3,tunerInstrument==0?PURPLE:PANEL);
  d.fillRoundRect(120,45,112,23,3,tunerInstrument==1?PURPLE:PANEL);
  d.setTextColor(WHITE,tunerInstrument==0?PURPLE:PANEL); d.setCursor(34,53); d.print("GUITAR");
  d.setTextColor(WHITE,tunerInstrument==1?PURPLE:PANEL); d.setCursor(151,53); d.print("UKULELE");
  d.setTextColor(CYAN,BG); d.setCursor(8,77); d.print("STANDARD TUNINGS");
  if(!tunerInstrument) {
    for(int i=0;i<6;i++){d.setTextColor(WHITE,BG);d.setCursor(8+i*38,94);d.printf("%s",guitarNames[i]);}
  } else {
    for(int i=0;i<4;i++){d.setTextColor(WHITE,BG);d.setCursor(22+i*52,94);d.printf("%s",ukeNames[i]);}
  }
  d.setTextColor(MUTED,BG); d.setCursor(8,108); d.print("Mic pitch tracking not enabled");
  footer("A/D  INSTRUMENT     ESC  BACK");
}
String parentPath(const String &p) {
  if(p=="/") return "/";
  int n=p.lastIndexOf('/');
  return n<=0?"/":p.substring(0,n);
}
void scanDirectory() {
  entryCount=0; selected=0; scroll=0;
  if(!sdReady)return;
  File dir=SD.open(currentPath);
  if(!dir || !dir.isDirectory()) { if(dir)dir.close(); return; }
  File f=dir.openNextFile();
  while(f && entryCount<48) {
    String name=String(f.name());
    int slash=name.lastIndexOf('/');
    if(slash>=0) name=name.substring(slash+1);
    if(name.length() && name!="System Volume Information") {
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
  d.setTextColor(CYAN,BG); d.setCursor(8,42);
  String path=currentPath; if(path.length()>27)path="..."+path.substring(path.length()-24);
  d.print(path);
  if(!sdReady) {
    d.setTextColor(PINK,BG); d.setCursor(8,68); d.print("NO SD CARD");
    d.setTextColor(MUTED,BG); d.setCursor(8,82); d.print("Check FAT32 card and contacts.");
    footer(); return;
  }
  int shown=0;
  if(currentPath!="/" && scroll==0) {
    d.setTextColor(MUTED,BG); d.setCursor(10,57); d.print("../");
    shown++;
  }
  for(int i=scroll;i<entryCount && shown<5;i++) {
    if(musicMode && entryIsDir[i]) continue;
    int y=56+shown*12;
    bool on=i==selected;
    d.fillRect(8,y-1,224,11,on?PANEL:BG);
    d.setTextColor(on?WHITE:(entryIsDir[i]?CYAN:MUTED),on?PANEL:BG);
    d.setCursor(12,y);
    String n=entries[i]; if(n.length()>29)n=n.substring(0,26)+"...";
    d.printf("%c %s%s",on?'>':' ',n.c_str(),entryIsDir[i]?"/":"");
    shown++;
  }
  if(entryCount==0){d.setTextColor(MUTED,BG);d.setCursor(10,68);d.print("EMPTY DIRECTORY");}
  footer("W/S MOVE  ENTER OPEN  BACKSPACE UP");
}
void drawMusicInfo(const String &name) {
  header("NOW SELECTED");
  auto &d=M5Cardputer.Display;
  d.fillRoundRect(69,43,100,54,5,PANEL);
  d.setTextColor(PINK,PANEL); d.setTextSize(3); d.setCursor(105,58); d.print("♪");
  d.setTextSize(1); d.setTextColor(WHITE,BG); d.setCursor(8,103);
  String n=name; if(n.length()>34)n=n.substring(0,31)+"...";
  d.print(n);
  d.setTextColor(MUTED,BG); d.setCursor(8,113); d.print("WAV / MP3 file browser; playback codec pending");
  footer("ESC  RETURN TO LIBRARY");
}
void openFile(bool musicMode) {
  if(!sdReady)return;
  if(currentPath!="/" && selected<0)return;
  if(currentPath!="/" && selected== -1) {currentPath=parentPath(currentPath);scanDirectory();drawFiles(musicMode);return;}
  if(selected<0 || selected>=entryCount)return;
  String full=currentPath;
  if(full!="/")full+="/";
  full+=entries[selected];
  if(entryIsDir[selected]) {currentPath=full;scanDirectory();drawFiles(musicMode);return;}
  selectedFile=full;
  if(musicMode) drawMusicInfo(entries[selected]);
  else {
    File f=SD.open(full);
    header("FILE DETAILS");
    auto &d=M5Cardputer.Display;
    d.setTextColor(WHITE,BG);d.setCursor(8,53);d.print(entries[selected].substring(0,30));
    d.setTextColor(MUTED,BG);d.setCursor(8,72);d.printf("%lu bytes",(unsigned long)f.size());
    d.setCursor(8,88);d.print("TYPE: ");d.print(f.isDirectory()?"DIR":"FILE");
    f.close();footer();
  }
}
void openApp() {
  switch(selected) {
    case 0:screen=PIANO;pianoScreen();break;
    case 1:screen=TUNER;tunerScreen();break;
    case 2:screen=MUSIC;currentPath="/";scanDirectory();drawFiles(true);break;
    case 3:screen=FILES;currentPath="/";scanDirectory();drawFiles();break;
    case 4:screen=GALLERY;header("PHOTO VIEWER");M5Cardputer.Display.setTextColor(MUTED,BG);M5Cardputer.Display.setCursor(8,61);M5Cardputer.Display.print("JPEG decoder not included");footer();break;
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
  if(!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()){delay(8);return;}
  auto k=M5Cardputer.Keyboard.keysState();
  char c=k.word.size()?k.word[0]:0;
  if(screen==HOME) {
    if(k.enter){openApp();}
    else if(c=='w'||c=='W'){selected=(selected+APP_COUNT-1)%APP_COUNT;drawHome();}
    else if(c=='s'||c=='S'){selected=(selected+1)%APP_COUNT;drawHome();}
  } else if(screen==PIANO) {
    if(c)playNote(c);
  } else if(screen==TUNER) {
    if(c=='a'||c=='A'||c=='d'||c=='D'){tunerInstrument=1-tunerInstrument;tunerScreen();}
  } else if(screen==FILES || screen==MUSIC) {
    bool music=screen==MUSIC;
    if(c=='w'||c=='W'){if(selected>0)selected--;if(selected<scroll)scroll=selected;drawFiles(music);}
    else if(c=='s'||c=='S'){if(selected<entryCount-1)selected++;if(selected>=scroll+5)scroll=selected-4;drawFiles(music);}
    else if(k.enter)openFile(music);
    else if(k.del){if(currentPath!="/"){currentPath=parentPath(currentPath);scanDirectory();drawFiles(music);}else{screen=HOME;drawHome();}}
  }
  if(k.del && screen!=HOME && screen!=FILES && screen!=MUSIC){screen=HOME;drawHome();}
  delay(12);
}
