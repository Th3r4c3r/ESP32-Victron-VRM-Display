/*
 * ESP32-Victron-VRM-Display
 * Victron VRM energy-flow dashboard on the ESP32-2432S028R ("Cheap Yellow Display").
 *
 * Replicates the VRM portal tile view (Grid / Inverter / AC Loads / Battery /
 * DC Loads / Solar) with live values from the Victron VRM API.
 *
 * The live values live in the VRM "diagnostics" endpoint (~129 KB JSON). They are
 * parsed in streaming with an ArduinoJson Filter so only the ~12 codes we need
 * stay in RAM. See the README "Technical notes" for the ESP32+TLS gotchas.
 *
 * Configure your credentials in secrets.h (copy secrets.example.h).
 *
 * Libraries: TFT_eSPI, ArduinoJson (v7), StreamUtils.
 * Board: ESP32 Dev Module (esp32:esp32:esp32).
 *
 * MIT License.
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <StreamUtils.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "secrets.h"

// ---------------- CONFIG ----------------
const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;
const char* VRM_TOKEN = SECRET_VRM_TOKEN;   // VRM Personal Access Token
const char* ID_SITE   = SECRET_VRM_SITE;    // VRM installation id (idSite)
const char* SITE_NAME = SECRET_SITE_NAME;   // label shown top-left
const long  TZOFF     = 7200;               // UTC offset in seconds (3600=CET, 7200=CEST)
const uint32_t POLL_MS = 20000;             // refresh interval (ms)
// ----------------------------------------

TFT_eSPI tft;

#define SCR_W 320
#define SCR_H 240

// VRM-style dark palette (RGB565)
#define BG      0x0000
#define BOX     0x10A2     // dark navy tiles
#define BORDER  0x3D7F     // steel-blue border
#define TITLE   0xACD3     // gray-blue
#define VAL     0xFFFF
#define UNIT    0x8C71
#define BLUE    0x2D9F     // battery fill
#define LINEC   0x4C9F     // flow lines
#define GREEN   0x2FE6
#define REDC    0xF800

// live values
double gridW=0, acW=0, dcW=0, battV=0, battA=0, battW=0, soc=0, battT=0, pvW=0;
String battState="-", sysState="-";
bool online=false;
uint32_t lastPoll=0, lastClock=0;
bool blink=false;

// tile geometry
struct Box { int x,y,w,h; };
Box bGrid = {4,  24,100,66};     // top row: Grid | Inverter | AC Loads (equal size)
Box bInv  = {110,24,100,66};
Box bAC   = {216,24,100,66};
Box bBatt = {4,  96,206,140};    // enlarged below
Box bDC   = {216,96,100,66};     // right column: DC Loads (top) + Solar (bottom)
Box bPV   = {216,170,100,66};

// Blocking stream wrapper: lets ArduinoJson read the whole TLS body instead of
// stopping at the first ~16 KB record (default Stream::read() is non-blocking).
struct BlockingStream : public Stream {
  WiFiClient* c;
  BlockingStream(WiFiClient* cc):c(cc){}
  int available() override { return c->available(); }
  int peek() override { return c->peek(); }
  size_t write(uint8_t) override { return 0; }
  int read() override {
    uint32_t t0=millis();
    while(true){
      int x=c->read();
      if(x>=0) return x;
      if(!c->connected() && c->available()==0) return -1;   // real EOF
      if(millis()-t0>20000) return -1;                       // safety guard
      delay(1);
    }
  }
};

String clockStr(){
  struct tm t; if(!getLocalTime(&t,30)) return "--:--";
  char b[6]; sprintf(b,"%02d:%02d",t.tm_hour,t.tm_min); return String(b);
}

void connectWiFi(){
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASS);
  tft.fillScreen(BG); tft.setTextColor(VAL,BG); tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting WiFi...",SCR_W/2,SCR_H/2,4);
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000) delay(250);
  Serial.println(WiFi.status()==WL_CONNECTED?"[WiFi] OK":"[WiFi] FAILED");
}

void drawBoxFrame(const Box&b,const char* title){
  tft.fillRoundRect(b.x,b.y,b.w,b.h,5,BOX);
  tft.drawRoundRect(b.x,b.y,b.w,b.h,5,BORDER);
  tft.setTextColor(TITLE,BOX); tft.setTextDatum(TL_DATUM);
  tft.drawString(title,b.x+6,b.y+5,2);
}

void drawConnector(int x1,int y1,int x2,int y2){
  tft.drawLine(x1,y1,x2,y2,LINEC);
  tft.fillCircle((x1+x2)/2,(y1+y2)/2,2,LINEC);
}

void drawFrame(){
  tft.fillScreen(BG);
  // header
  tft.setTextColor(TITLE,BG); tft.setTextDatum(ML_DATUM);
  tft.drawString(SITE_NAME,4,11,2);
  // tiles
  drawBoxFrame(bGrid,"Grid");
  drawBoxFrame(bInv ,"Inverter");
  drawBoxFrame(bAC  ,"AC Loads");
  drawBoxFrame(bBatt,"Battery");
  drawBoxFrame(bDC  ,"DC Loads");
  drawBoxFrame(bPV  ,"Solar");
  // flow connectors
  drawConnector(bGrid.x+bGrid.w, bGrid.y+33, bInv.x, bGrid.y+33);        // Grid-Inverter
  drawConnector(bInv.x+bInv.w,   bInv.y+33, bAC.x,  bInv.y+33);          // Inverter-AC
  drawConnector(bInv.x+bInv.w/2, bInv.y+bInv.h, bInv.x+bInv.w/2, bBatt.y); // Inverter-Battery
  drawConnector(bBatt.x+bBatt.w, bDC.y+33, bDC.x,  bDC.y+33);            // Battery-DC
  drawConnector(bBatt.x+bBatt.w, bPV.y+33, bPV.x,  bPV.y+33);            // Battery-Solar
}

// draw a "NNN W" value inside a tile (clears the area first)
void drawWatt(const Box&b,int cy,double w){
  tft.fillRect(b.x+3,cy-16,b.w-6,32,BOX);
  String n=String((long)round(w));
  tft.setTextColor(VAL,BOX); tft.setTextDatum(ML_DATUM);
  tft.drawString(n,b.x+8,cy,4);
  int wn=tft.textWidth(n,4);
  tft.setTextColor(UNIT,BOX); tft.drawString("W",b.x+8+wn+4,cy,2);
}

void drawBattery(){
  Box b=bBatt;
  // temperature top-right: number + degree ring + C
  int cx=b.x+b.w-8;
  tft.fillRect(b.x+b.w-60,b.y+5,56,16,BOX);
  tft.setTextColor(TITLE,BOX); tft.setTextDatum(TR_DATUM);
  tft.drawString("C",cx,b.y+6,2);
  int wc=tft.textWidth("C",2);
  tft.drawCircle(cx-wc-3,b.y+9,2,TITLE);
  tft.drawString(String((int)round(battT)),cx-wc-7,b.y+6,2);
  // big SoC (font 7, 7-seg, ~48px)
  tft.fillRect(b.x+3,b.y+22,b.w-6,52,BOX);
  String s=String((int)round(soc));
  tft.setTextColor(VAL,BOX); tft.setTextDatum(TL_DATUM);
  tft.drawString(s,b.x+12,b.y+22,7);
  int ws=tft.textWidth(s,7);
  tft.setTextColor(UNIT,BOX); tft.drawString("%",b.x+18+ws,b.y+44,4);
  // state
  tft.fillRect(b.x+3,b.y+78,b.w-6,16,BOX);
  tft.setTextColor(0x6E5F,BOX); tft.setTextDatum(TL_DATUM);
  tft.drawString(battState,b.x+12,b.y+78,2);
  // SoC bar (wide)
  int bx=b.x+12, by=b.y+98, bw=b.w-24, bh=18;
  tft.drawRect(bx,by,bw,bh,BORDER);
  tft.fillRect(bx+1,by+1,bw-2,bh-2,BOX);
  int fw=(int)((bw-2)*constrain(soc,0,100)/100.0);
  tft.fillRect(bx+1,by+1,fw,bh-2,BLUE);
  // V / A / W
  tft.fillRect(b.x+3,b.y+120,b.w-6,18,BOX);
  tft.setTextColor(TITLE,BOX); tft.setTextDatum(TL_DATUM);
  char line[48];
  sprintf(line,"%.2fV    %.1fA    %dW",battV,battA,(int)round(battW));
  tft.drawString(line,b.x+12,b.y+121,2);
}

void drawInverterState(){
  Box b=bInv;
  tft.fillRect(b.x+3,b.y+24,b.w-6,38,BOX);
  tft.setTextColor(VAL,BOX); tft.setTextDatum(TL_DATUM);
  String s=sysState;
  if(tft.textWidth(s,4)<=b.w-12){ tft.drawString(s,b.x+8,b.y+30,4); }
  else { tft.drawString(s,b.x+8,b.y+34,2); }
}

void updateValues(){
  drawWatt(bGrid, bGrid.y+44, gridW);
  drawWatt(bAC,   bAC.y+44, acW);
  drawWatt(bDC,   bDC.y+44, dcW);
  drawWatt(bPV,   bPV.y+44, pvW);
  drawInverterState();
  drawBattery();
}

void drawWifi(int x,int y,bool up,int rssi){
  int lvl=0; if(up){ if(rssi>-60)lvl=4; else if(rssi>-70)lvl=3; else if(rssi>-80)lvl=2; else lvl=1; }
  for(int i=0;i<4;i++){ int h=3+i*3; uint16_t c=up?(i<lvl?GREEN:0x4208):REDC; tft.fillRect(x+i*5,y+(12-h),3,h,c); }
}

void updateHeader(){
  tft.fillRect(SCR_W/2-30,0,60,20,BG);
  tft.setTextColor(online?VAL:UNIT,BG); tft.setTextDatum(MC_DATUM);
  tft.drawString(clockStr(),SCR_W/2,11,4);
  tft.fillRect(SCR_W-30,2,28,16,BG);
  drawWifi(SCR_W-26,3,online,WiFi.RSSI());
  tft.fillCircle(SCR_W-34,10,2,blink?LINEC:BG);
}

bool fetchData(){
  WiFiClientSecure c; c.setInsecure(); c.setTimeout(20000);
  HTTPClient http; http.setTimeout(20000);
  String url=String("https://vrmapi.victronenergy.com/v2/installations/")+ID_SITE+"/diagnostics?count=1000";
  if(!http.begin(c,url)) return false;
  http.useHTTP10(true);            // avoid chunked encoding -> clean stream for ArduinoJson
  http.addHeader("x-authorization", String("Token ")+VRM_TOKEN);
  int code=http.GET();
  if(code!=200){ Serial.printf("[VRM] HTTP %d\n",code); http.end(); return false; }

  // Filter: keep only code + rawValue + formattedValue of each record (saves RAM)
  JsonDocument filter;
  filter["records"][0]["code"]=true;
  filter["records"][0]["rawValue"]=true;
  filter["records"][0]["formattedValue"]=true;

  JsonDocument doc;
  BlockingStream bs(http.getStreamPtr());
  DeserializationError e=deserializeJson(doc,bs,DeserializationOption::Filter(filter));
  http.end();
  if(e){ Serial.printf("[VRM] JSON err %s\n",e.c_str()); return false; }

  for(JsonObject r: doc["records"].as<JsonArray>()){
    const char* code=r["code"]; if(!code) continue;
    if      (!strcmp(code,"g1"))  gridW=r["rawValue"]|0.0;   // Grid L1 power
    else if (!strcmp(code,"a1"))  acW =r["rawValue"]|0.0;    // AC consumption L1
    else if (!strcmp(code,"dc"))  dcW =r["rawValue"]|0.0;    // DC system power
    else if (!strcmp(code,"bv"))  battV=r["rawValue"]|0.0;   // battery voltage
    else if (!strcmp(code,"bc"))  battA=r["rawValue"]|0.0;   // battery current
    else if (!strcmp(code,"bp"))  battW=r["rawValue"]|0.0;   // battery power
    else if (!strcmp(code,"bs"))  soc =r["rawValue"]|0.0;    // battery SOC
    else if (!strcmp(code,"bT"))  battT=r["rawValue"]|0.0;   // battery temperature
    else if (!strcmp(code,"PVP")) pvW =r["rawValue"]|0.0;    // PV power
    else if (!strcmp(code,"bst")) battState=(const char*)(r["formattedValue"]|"-"); // battery state
    else if (!strcmp(code,"ss"))  sysState =(const char*)(r["formattedValue"]|"-"); // system state
  }
  Serial.printf("[VRM] grid=%.0f ac=%.0f dc=%.0f soc=%.0f %s | %s\n",
                gridW,acW,dcW,soc,battState.c_str(),sysState.c_str());
  return true;
}

void setup(){
  Serial.begin(115200);
  pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,HIGH);
  tft.init(); tft.setRotation(1);
  tft.invertDisplay(true);          // many CYD panels are inverted; flip to false if colors look wrong
  tft.fillScreen(BG);
  connectWiFi();
  configTime(TZOFF,0,"pool.ntp.org","time.google.com");
  drawFrame();
  online=fetchData();
  updateHeader();
  updateValues();
  lastPoll=millis();
}

void loop(){
  uint32_t now=millis();
  if(now-lastClock>1000){ lastClock=now; blink=!blink; updateHeader(); }
  if(now-lastPoll>POLL_MS){
    lastPoll=now;
    online=fetchData();
    updateHeader();
    if(online) updateValues();
  }
  if(WiFi.status()!=WL_CONNECTED){ WiFi.begin(WIFI_SSID,WIFI_PASS); delay(300); }
  delay(20);
}
