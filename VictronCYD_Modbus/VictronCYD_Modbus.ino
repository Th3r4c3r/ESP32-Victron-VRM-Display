/*
 * ESP32-Victron-VRM-Display — REALTIME variant (local Modbus TCP)
 * Reads the values straight from the GX device (Cerbo/Venus) on your LAN via
 * Modbus TCP. No cloud, no token, ~2 s refresh, works without internet.
 * Same layout as the VRM-cloud version.
 *
 * On the GX: Settings -> Services -> Modbus TCP = ON.
 * Configure WiFi + GX IP in secrets.h (copy secrets.example.h).
 *
 * Library: TFT_eSPI only. Board: ESP32 Dev Module.
 * MIT License.
 */
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <time.h>
#include "esp_task_wdt.h"
#include "secrets.h"

#define WDT_TIMEOUT_S 30        // reboot if the loop hangs for >30s
#define NODATA_REBOOT_MS 120000 // reboot if no valid data for 2 min

// ---------------- CONFIG ----------------
const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;
const char* GX_IP     = SECRET_GX_IP;       // GX (Cerbo/Venus) IP address
const char* SITE_NAME = SECRET_SITE_NAME;   // label shown top-left
const long  TZOFF     = 7200;               // UTC offset in seconds
const uint32_t POLL_MS = 2000;              // realtime refresh
// ----------------------------------------

TFT_eSPI tft;
WiFiClient mb;
uint16_t mbTid = 0;

#define SCR_W 320
#define SCR_H 240
#define BG      0x0000
#define BOX     0x10A2
#define BORDER  0x3D7F
#define TITLE   0xACD3
#define VAL     0xFFFF
#define UNIT    0x8C71
#define BLUE    0x2D9F
#define LINEC   0x4C9F
#define GREEN   0x2FE6
#define REDC    0xF800

double gridW=0, acW=0, dcW=0, battV=0, battA=0, battW=0, soc=0, battT=0, pvW=0;
String battState="-", sysState="-";
bool online=false;
uint32_t lastPoll=0, lastClock=0, lastGood=0;
bool blink=false;

struct Box { int x,y,w,h; };
Box bGrid={4,24,100,66}, bInv={110,24,100,66}, bAC={216,24,100,66};
Box bBatt={4,96,206,140}, bDC={216,96,100,66}, bPV={216,170,100,66};

int s16(uint16_t v){ return v>32767 ? (int)v-65536 : (int)v; }

// ---- Modbus TCP read holding registers (synchronous, robust) ----
// On any anomaly it closes the socket so the next read reconnects and
// re-syncs by itself (avoids TCP buffer desync that can freeze the display).
bool mbRead(uint8_t unit, uint16_t addr, uint16_t cnt, uint16_t* out){
  if(!mb.connected()){ mb.stop(); if(!mb.connect(GX_IP,502,3000)) return false; }
  while(mb.available()) mb.read();          // drain stale bytes (anti-desync)
  mbTid++;
  uint8_t req[12] = { (uint8_t)(mbTid>>8),(uint8_t)mbTid, 0,0, 0,6, unit,
                      0x03,(uint8_t)(addr>>8),(uint8_t)addr,(uint8_t)(cnt>>8),(uint8_t)cnt };
  if(mb.write(req,12)!=12){ mb.stop(); return false; }
  uint32_t t0=millis();
  while(mb.available()<9){ if(!mb.connected()||millis()-t0>800){ mb.stop(); return false; } delay(1); }
  uint8_t h[9];
  if(mb.readBytes(h,9)!=9){ mb.stop(); return false; }
  uint16_t rtid=(h[0]<<8)|h[1];
  if(rtid!=mbTid || h[7]!=0x03 || h[8]!=cnt*2 || h[8]>64){ mb.stop(); return false; }  // desync/exception
  uint8_t bc=h[8], d[64];
  t0=millis();
  while((int)mb.available()<bc){ if(!mb.connected()||millis()-t0>800){ mb.stop(); return false; } delay(1); }
  if(mb.readBytes(d,bc)!=bc){ mb.stop(); return false; }
  for(int i=0;i<cnt;i++) out[i]=(d[i*2]<<8)|d[i*2+1];
  return true;
}

String battStateStr(int c){ return c==0?"idle":c==1?"charging":c==2?"discharging":"-"; }
String vebusStr(int s){
  switch(s){
    case 0:return "Off"; case 1:return "Low power"; case 2:return "Fault";
    case 3:return "Bulk"; case 4:return "Absorption"; case 5:return "Float";
    case 6:return "Storage"; case 7:return "Equalize"; case 8:return "Passthru";
    case 9:return "Inverting"; case 10:return "Power assist"; case 11:return "Power supply";
    case 244:return "Sustain"; case 252:return "Ext. control";
    default:return String(s);
  }
}

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
  tft.setTextColor(TITLE,BG); tft.setTextDatum(ML_DATUM);
  tft.drawString(SITE_NAME,4,11,2);
  drawBoxFrame(bGrid,"Grid"); drawBoxFrame(bInv,"Inverter"); drawBoxFrame(bAC,"AC Loads");
  drawBoxFrame(bBatt,"Battery"); drawBoxFrame(bDC,"DC Loads"); drawBoxFrame(bPV,"Solar");
  drawConnector(bGrid.x+bGrid.w, bGrid.y+33, bInv.x, bGrid.y+33);
  drawConnector(bInv.x+bInv.w,   bInv.y+33, bAC.x,  bInv.y+33);
  drawConnector(bInv.x+bInv.w/2, bInv.y+bInv.h, bInv.x+bInv.w/2, bBatt.y);
  drawConnector(bBatt.x+bBatt.w, bDC.y+33, bDC.x,  bDC.y+33);
  drawConnector(bBatt.x+bBatt.w, bPV.y+33, bPV.x,  bPV.y+33);
}
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
  int cx=b.x+b.w-8;
  tft.fillRect(b.x+b.w-60,b.y+5,56,16,BOX);
  tft.setTextColor(TITLE,BOX); tft.setTextDatum(TR_DATUM);
  tft.drawString("C",cx,b.y+6,2);
  int wc=tft.textWidth("C",2);
  tft.drawCircle(cx-wc-3,b.y+9,2,TITLE);
  tft.drawString(String((int)round(battT)),cx-wc-7,b.y+6,2);
  tft.fillRect(b.x+3,b.y+22,b.w-6,52,BOX);
  String s=String((int)round(soc));
  tft.setTextColor(VAL,BOX); tft.setTextDatum(TL_DATUM);
  tft.drawString(s,b.x+12,b.y+22,7);
  int ws=tft.textWidth(s,7);
  tft.setTextColor(UNIT,BOX); tft.drawString("%",b.x+18+ws,b.y+44,4);
  tft.fillRect(b.x+3,b.y+78,b.w-6,16,BOX);
  tft.setTextColor(0x6E5F,BOX); tft.setTextDatum(TL_DATUM);
  tft.drawString(battState,b.x+12,b.y+78,2);
  int bx=b.x+12, by=b.y+98, bw=b.w-24, bh=18;
  tft.drawRect(bx,by,bw,bh,BORDER);
  tft.fillRect(bx+1,by+1,bw-2,bh-2,BOX);
  int fw=(int)((bw-2)*constrain(soc,0,100)/100.0);
  tft.fillRect(bx+1,by+1,fw,bh-2,BLUE);
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
  if(tft.textWidth(s,4)<=b.w-12) tft.drawString(s,b.x+8,b.y+30,4);
  else tft.drawString(s,b.x+8,b.y+34,2);
}
void updateValues(){
  drawWatt(bGrid,bGrid.y+44,gridW);
  drawWatt(bAC,bAC.y+44,acW);
  drawWatt(bDC,bDC.y+44,dcW);
  drawWatt(bPV,bPV.y+44,pvW);
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

// GX Modbus register map — com.victronenergy.system is unit id 100.
bool fetchData(){
  uint16_t r[8]; bool ok=true;
  if(mbRead(100,817,4,r)){ acW=s16(r[0]); gridW=s16(r[3]); } else ok=false;   // 817 AC L1..L3, 820 Grid L1
  if(mbRead(100,840,5,r)){ battV=r[0]/10.0; battA=s16(r[1])/10.0; battW=s16(r[2]);
                           soc=(r[3]>100? r[3]/10.0 : r[3]); battState=battStateStr(r[4]); } else ok=false;
  uint16_t v[1];
  if(mbRead(100,850,1,v)) pvW=v[0];                       // PV DC-coupled power
  if(mbRead(100,860,1,v)) dcW=s16(v[0]);                  // DC system power
  if(mbRead(228,31,1,v))  sysState=vebusStr(v[0]);        // VE.Bus state (unit 228)
  if(mbRead(225,262,1,v)) battT=s16(v[0])/10.0;           // battery temperature (unit 225, /10)
  Serial.printf("[MB] grid=%.0f ac=%.0f dc=%.0f pv=%.0f soc=%.0f %s | %s\n",
                gridW,acW,dcW,pvW,soc,battState.c_str(),sysState.c_str());
  return ok;
}

void setup(){
  Serial.begin(115200);
  pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,HIGH);
  tft.init(); tft.setRotation(1); tft.invertDisplay(true); tft.fillScreen(BG);
  connectWiFi();
  configTime(TZOFF,0,"pool.ntp.org","time.google.com");
  drawFrame();
  online=fetchData();
  updateHeader(); updateValues();
  // hardware watchdog (set up after the initial connect, which can be slow)
  esp_task_wdt_config_t twdt = { .timeout_ms = (uint32_t)WDT_TIMEOUT_S*1000, .idle_core_mask = 0, .trigger_panic = true };
  if(esp_task_wdt_init(&twdt)==ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&twdt);
  esp_task_wdt_add(NULL);
  lastGood=millis();
  lastPoll=millis();
}

void loop(){
  esp_task_wdt_reset();                 // feed the hardware watchdog
  uint32_t now=millis();
  if(now-lastClock>1000){ lastClock=now; blink=!blink; updateHeader(); }
  if(now-lastPoll>POLL_MS){
    lastPoll=now;
    online=fetchData();
    if(online) lastGood=now;
    updateHeader();
    if(online) updateValues();
  }
  if(WiFi.status()!=WL_CONNECTED){ WiFi.begin(WIFI_SSID,WIFI_PASS); delay(300); }
  if(millis()-lastGood>NODATA_REBOOT_MS) ESP.restart();   // no fresh data for too long -> reboot
  delay(20);
}
