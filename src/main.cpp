#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <time.h>
#include <esp_sntp.h>
#include "noaa_ca.h"

namespace {
constexpr int W = 480, H = 320;
// E32R35T: red LED is GPIO22; GPIO4 is the active-low audio amplifier enable.
constexpr int BL = 27, LED_R = 22, LED_G = 16, LED_B = 17, BOOT = 0;
constexpr int AUDIO_EN = 4, BATTERY_ADC = 34;
int batteryMv = 0, batteryPercent = -1;
bool batteryValid = false;
float batterySmoothMv = 0;
uint32_t lastBatterySample = 0, redTestUntil = 0;
String diagnosticCommand;

// A generic single-cell LiPo approximation, not a measured state of charge.
int percentFromBatteryMv(int mv) {
  const int volts[] = {3300,3500,3600,3700,3800,3900,4000,4100,4200};
  const int levels[] = {0,5,10,20,40,60,80,90,100};
  if(mv<=volts[0]) return 0;
  for(int i=1;i<9;++i) if(mv<=volts[i])
    return levels[i-1]+(mv-volts[i-1])*(levels[i]-levels[i-1])/(volts[i]-volts[i-1]);
  return 100;
}

void sampleBattery(bool force=false) {
  if(!force && millis()-lastBatterySample<3000) return;
  lastBatterySample=millis();
  uint32_t sum=0;
  for(int i=0;i<16;++i) sum+=analogReadMilliVolts(BATTERY_ADC);
  int candidateMv=static_cast<int>((sum*2+8)/16); // Existing 100k/100k divider.
  batteryValid=candidateMv>=2500 && candidateMv<=4500;
  if(!batteryValid) {batteryMv=0; batteryPercent=-1; batterySmoothMv=0; return;}
  batterySmoothMv=batterySmoothMv?batterySmoothMv*0.75f+candidateMv*0.25f:candidateMv;
  batteryMv=static_cast<int>(roundf(batterySmoothMv));
  batteryPercent=percentFromBatteryMv(batteryMv);
}

void logBattery() {
  if(batteryValid) Serial.printf("Battery GPIO34: %d mV, estimated %d%%\n",batteryMv,batteryPercent);
  else Serial.println("Battery GPIO34: reading unavailable / outside valid range");
}

void setupBattery() {
  pinMode(BATTERY_ADC,INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC,ADC_11db);
  sampleBattery(true); logBattery();
  bool ok=percentFromBatteryMv(3300)==0 && percentFromBatteryMv(4200)==100 &&
          percentFromBatteryMv(3750)==30 && percentFromBatteryMv(5000)==100;
  for(int mv=3301;mv<=4200;++mv) ok=ok && percentFromBatteryMv(mv)>=percentFromBatteryMv(mv-1);
  Serial.printf("Battery curve checks: %s\n",ok?"PASS":"FAIL");
}
uint16_t BG = 0x0843, PANEL = 0x10A5, WHITE = 0xEF7D;
uint16_t MUTED = 0x8C71, CYAN = 0x45FA, AMBER = 0xFCA1;
uint16_t GREEN = 0x67EC, RED = 0xFA69;
uint8_t theme = 2;
bool nightMode = false, autoNight = false, idleCycle = false;
int16_t utcMinutes = 0;
uint8_t zonePreset = 0, lampMode = 0, gradientSpeed = 1;
uint8_t spotlightSeconds = 8, quietStart = 22, quietEnd = 6;
bool quietHours = false;
volatile bool clockSynced = false;
uint32_t clockSyncAt = 0, lastBanner = 0;
uint8_t bandFilter = 0, modeFilter = 0, spotPage = 0;
constexpr int SPOT_CAPACITY = 30;
uint8_t visibleSpots[5], visibleCount = 0;
String spaceWeather = "NOAA: waiting", spaceStamp;
uint32_t nextSpace = 0, lastSpace = 0;
uint8_t loginStage = 0;
uint32_t loginDeadline = 0;
const char *ZONES[] = {"FIXED OFFSET", "US EASTERN", "US CENTRAL", "US MOUNTAIN", "US PACIFIC"};
const char *TZ_RULES[] = {"UTC0", "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0"};
void configureZone() { setenv("TZ", TZ_RULES[zonePreset], 1); tzset(); }
void localParts(time_t epoch, struct tm &parts) {
  if (zonePreset) localtime_r(&epoch, &parts);
  else { epoch += utcMinutes * 60; gmtime_r(&epoch, &parts); }
}
String localStamp(time_t epoch, bool date = false) {
  if (epoch < 1600000000) return "Time unavailable";
  struct tm parts; localParts(epoch, parts); char buf[32];
  strftime(buf, sizeof(buf), date ? "%m/%d %I:%M %p" : "%I:%M %p", &parts);
  return String(buf);
}
String offsetLabel() {
  char buf[20]; snprintf(buf, sizeof(buf), "UTC%c%02d:%02d", utcMinutes < 0 ? '-' : '+', abs(utcMinutes)/60, abs(utcMinutes)%60);
  return String(buf);
}
bool inQuietHours() {
  if (!quietHours || time(nullptr) < 1600000000) return false;
  struct tm parts; localParts(time(nullptr), parts);
  return quietStart < quietEnd ? parts.tm_hour >= quietStart && parts.tm_hour < quietEnd : parts.tm_hour >= quietStart || parts.tm_hour < quietEnd;
}
void onTimeSync(struct timeval *) { clockSynced = true; clockSyncAt = millis(); }
time_t parseSpotTime(String value, time_t received) {
  value.trim();
  if(value.length()==4 && value[0]>='0' && value[0]<='9' && value[1]>='0' && value[1]<='9' && value[2]>='0' && value[2]<='9' && value[3]>='0' && value[3]<='9') value=value.substring(0,2)+":"+value.substring(2);
  if (value.length() >= 10 && value.indexOf('-') < 0 && value.indexOf(':') < 0) {
    char *end; long long n = strtoll(value.c_str(), &end, 10);
    if (*end == 0) { if (n > 100000000000LL) n /= 1000; if (n >= 1600000000LL && n < 4102444800LL) return n; }
  }
  struct tm parts = {}; int y,m,d,h,minute,sec=0;
  if (sscanf(value.c_str(), "%d-%d-%dT%d:%d:%d", &y,&m,&d,&h,&minute,&sec) >= 5 ||
      sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &y,&m,&d,&h,&minute,&sec) >= 5) {
    if (y<2020 || y>2099 || m<1 || m>12 || d<1 || d>31 || h<0 || h>23 || minute<0 || minute>59 || sec<0 || sec>60) return 0;
    parts.tm_year=y-1900; parts.tm_mon=m-1; parts.tm_mday=d; parts.tm_hour=h; parts.tm_min=minute; parts.tm_sec=sec;
    // UTC civil date to epoch, independent of configured display timezone.
    int year=y-(m<=2), era=year/400; unsigned yo=year-era*400;
    unsigned day=(153*(m+(m>2?-3:9))+2)/5+d-1;
    time_t result=(era*146097L+yo*365+yo/4-yo/100+day-719468L)*86400LL+h*3600+minute*60+sec;
    int signAt=max(value.indexOf('+',19), value.indexOf('-',19));
    if (signAt>=0) { int oh=0,om=0; if(sscanf(value.substring(signAt+1).c_str(),"%d:%d",&oh,&om)>=1) result+=(value[signAt]=='+'?-1:1)*(oh*3600+om*60); }
    return result;
  }
  value.replace("Z", ""); value.replace(" UTC", "");
  if (received >= 1600000000 && sscanf(value.c_str(), "%d:%d:%d", &h,&minute,&sec)>=2 && h>=0 && h<24 && minute>=0 && minute<60 && sec>=0 && sec<60) {
    time_t result=(received/86400)*86400+h*3600+minute*60+sec;
    if (result>received+300) result-=86400; return result;
  }
  return 0;
}

void checkTimeFormatting() {
  uint8_t savedZone=zonePreset; int16_t savedMinutes=utcMinutes; int passed=0,total=0;
  auto check=[&](bool ok) {++total; if(ok) ++passed;};
  const time_t jan=parseSpotTime("2026-01-15T12:00:00Z",0);
  const time_t jul=parseSpotTime("2026-07-15T12:00:00Z",0);
  check(jan==1768478400); check(jul==1784116800);
  check(parseSpotTime("2026-01-15T07:00:00-05:00",0)==jan);
  check(parseSpotTime("1768478400000",0)==jan);
  check(parseSpotTime("1200",jan)==jan);
  check(parseSpotTime("23:59Z",jan-12*3600)==jan-12*3600-60);
  check(parseSpotTime("invalid",jan)==0); check(parseSpotTime("25:10",jan)==0);
  zonePreset=0; utcMinutes=330; configureZone(); check(localStamp(jan)=="05:30 PM");
  utcMinutes=-720; check(localStamp(jan)=="12:00 AM");
  zonePreset=2; configureZone(); check(localStamp(jan)=="06:00 AM"); check(localStamp(jul)=="07:00 AM");
  zonePreset=savedZone; utcMinutes=savedMinutes; configureZone();
  Serial.printf("Time checks: %d/%d passed\n",passed,total);
}
uint8_t normalBacklight = 255, nightBacklight = 35;
uint32_t lastInteraction = 0, lastIdlePage = 0, spotlightUntil = 0;
bool spotlightOpen = false, trendExpanded = false, idleActive = false;
uint8_t idlePage = 0;
TFT_eSPI tft;
TFT_eSprite radarCanvas(&tft);
constexpr int RADAR_X = 25, RADAR_Y = 42, RADAR_W = 244, RADAR_H = 230;
bool radarCanvasReady = false;
WebServer web(80);
Preferences prefs;
WiFiClient alertClient;
String setupPassword, meshToken, wifiSsid, wifiPassword, propUrl;
String hamUser, hamPassword, hamHost;
uint16_t hamPort = 7300;
bool lampEnabled = true, apMode = false, gt911 = false, touchReady = false;
uint8_t lampPercent = 25;
String scanSsids[5];
int32_t scanRssi[5] = {};
bool scanSecured[5] = {};
uint8_t scanCount = 0;
bool wifiScanView = false, scanSelectionPending = false;
bool scanInProgress = false, scanPressLocked = false;
bool scanFailed = false;
uint32_t scanStartedAt = 0;
struct Rgb { uint8_t r, g, b; };
constexpr Rgb RGB_PALETTE[] = {{255,0,0}, {255,100,0}, {255,255,0},
                              {140,255,0}, {0,255,0}, {0,255,255},
                              {0,130,255}, {0,0,255}, {140,0,255},
                              {255,0,255}, {255,255,255}, {0,0,0}};
constexpr const char *RGB_NAMES[] = {"RED","ORANGE","YELLOW","LIME","GREEN","CYAN",
                                    "SKY BLUE","BLUE","VIOLET","MAGENTA","WHITE","OFF"};
constexpr int PALETTE_COUNT = sizeof(RGB_PALETTE) / sizeof(RGB_PALETTE[0]);
uint8_t tabColor[5] = {0, 9, 6, 6, 7};
bool propAlerts = true, spotAlerts = true, meshAlerts = true;
uint8_t openingScore = 70;
uint8_t page = 0;
uint8_t settingsView = 0, lightTab = 0, editField = 0, keyboardMode = 0;
int8_t ledTest = -1;
String editValue;
String editorError;
bool keyboardOpen = false;
uint32_t nextProp = 0, nextAlertConnect = 0;
uint32_t nextWifiRetry = 0;
uint32_t wifiConnectStarted = 0;
bool wifiConnecting = false;
uint32_t lastPropSuccess = 0;
uint32_t lastHealthRefresh = 0;
uint32_t nextRadar = 0, lastRadarSuccess = 0;
uint32_t lastRadarFrame = 0;
uint32_t alertUntil = 0, lastAlertKeepalive = 0, touchTime = 0, bootHold = 0, lastTouchLog = 0;
bool bootLong = false;
uint8_t alertR = 0, alertG = 0, alertB = 0;
bool propLive = false, rfConnected = false, aprsConnected = false, opening = false;
float myScore = 0, regionalScore = 0;
int directCount = 0, regionalCount = 0;
String myLevel = "WAITING", regionalLevel = "WAITING", station = "";
String eventState = "normal";
float stationLat = 0, stationLon = 0;
bool stationLocationReady = false;
float manualLat = 0, manualLon = 0;
bool manualLatSet = false, manualLonSet = false, manualRadar = false;

bool validCoordinate(const String &value, float limit, float &result) {
  if (value.isEmpty()) return false;
  char *end = nullptr;
  double parsed = strtod(value.c_str(), &end);
  if (end == value.c_str() || *end != '\0' || !isfinite(parsed) || parsed < -limit || parsed > limit)
    return false;
  result = static_cast<float>(parsed);
  return true;
}

constexpr int MAX_AIRCRAFT = 32;
struct Aircraft {
  char hex[9], flight[12];
  float distance, bearing, track;
  int altitude, speed;
};
Aircraft aircraft[MAX_AIRCRAFT], oldAircraft[MAX_AIRCRAFT];
uint8_t aircraftCount = 0, oldAircraftCount = 0, radarRadiusNm = 50;
String selectedAircraft, radarState = "WAITING";
void resetRadarCenter() {
  aircraftCount = 0;
  oldAircraftCount = 0;
  selectedAircraft = "";
  radarState = "UPDATING";
  nextRadar = millis();
}
constexpr int TREND_SAMPLES = 60;
uint8_t myTrend[TREND_SAMPLES] = {}, regionalTrend[TREND_SAMPLES] = {};
uint8_t trendCount = 0, trendNext = 0;
uint8_t openingTrend[TREND_SAMPLES] = {};
struct PropSnapshot {
  bool live, rf, aprs;
  float myScore, regionalScore;
  int directCount, regionalCount;
  String myLevel, regionalLevel, station, eventState;
};
String alertLine;
struct Spot {
  String call, freq, mode, spotter, location, comment, spotTime, receivedUtc;
  time_t spotEpoch = 0, receivedEpoch = 0;
  uint32_t receivedAt = 0;
};
Spot spots[SPOT_CAPACITY];
uint8_t spotCount = 0;
int8_t selectedSpot = -1;
bool unreadSpot = false;
struct TrailPoint { float distance, bearing; uint32_t when; };
TrailPoint trails[MAX_AIRCRAFT][4] = {};
char trailHex[MAX_AIRCRAFT][9] = {};
uint8_t trailCount[MAX_AIRCRAFT] = {};
struct MeshEvent { String from, text, type, signal; };
MeshEvent meshEvents[5];
uint8_t meshCount = 0;
uint32_t lastMesh = 0;

String htmlEscape(const String &s) {
  String out;
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String randomHex(size_t chars) {
  String value;
  const char *digits = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  for (size_t i = 0; i < chars; ++i) value += digits[esp_random() % 32];
  return value;
}

void applyTheme() {
  // Classic green, amber terminal, and cyan instrument presets.
  if (theme == 0) {
    BG = 0x0020; PANEL = 0x0840; WHITE = 0xCFF7; MUTED = 0x5A4A;
    CYAN = 0x47E8; GREEN = 0x67E8; AMBER = 0xCF40; RED = 0xF9C7;
  } else if (theme == 1) {
    BG = 0x1000; PANEL = 0x20A0; WHITE = 0xFF37; MUTED = 0x9B25;
    CYAN = 0xFD20; GREEN = 0xD5E0; AMBER = 0xFE20; RED = 0xF9C7;
  } else {
    BG = 0x0843; PANEL = 0x10A5; WHITE = 0xEF7D; MUTED = 0x8C71;
    CYAN = 0x45FA; GREEN = 0x67EC; AMBER = 0xFCA1; RED = 0xFA69;
  }
  if (nightMode) {
    BG = 0x0000; PANEL = 0x0800;
    MUTED = 0x528A;
  }
  ledcWrite(3, nightMode ? nightBacklight : normalBacklight);
}

void updateNightSchedule() {
  if (!autoNight) return;
  time_t now = time(nullptr);
  if (now < 1600000000) return;
  struct tm parts; localParts(now, parts); int hour = parts.tm_hour;
  bool shouldDim = hour >= 22 || hour < 6;
  if (shouldDim != nightMode) { nightMode = shouldDim; applyTheme(); }
}

void saveSettings() {
  prefs.putString("setup", setupPassword);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("wifi", wifiPassword);
  prefs.putString("prop", propUrl);
  prefs.putString("hamuser", hamUser);
  prefs.putString("hampass", hamPassword);
  prefs.putString("hamhost", hamHost);
  prefs.putUShort("hamport", hamPort);
  prefs.putBool("lamp", lampEnabled);
  prefs.putUChar("lampPct", lampPercent);
  prefs.putBytes("tabcolor", tabColor, sizeof(tabColor));
  prefs.putUChar("palettever",1);
  prefs.putBool("propalert", propAlerts);
  prefs.putBool("spotalert", spotAlerts);
  prefs.putBool("meshalert", meshAlerts);
  prefs.putUChar("openscore", openingScore);
  prefs.putUChar("theme", theme);
  prefs.putBool("night", nightMode);
  prefs.putBool("autonight", autoNight);
  prefs.putShort("utcmins", utcMinutes);
  prefs.putUChar("zone", zonePreset); prefs.putUChar("lampmode", lampMode);
  prefs.putUChar("speed", gradientSpeed); prefs.putUChar("spotsec", spotlightSeconds);
  prefs.putBool("quiet", quietHours); prefs.putUChar("qstart", quietStart); prefs.putUChar("qend", quietEnd);
  prefs.putBool("idlecycle", idleCycle);
  prefs.putUChar("daybl", normalBacklight);
  prefs.putUChar("nightbl", nightBacklight);
  if (manualLatSet) prefs.putFloat("radlat", manualLat);
  if (manualLonSet) prefs.putFloat("radlon", manualLon);
  prefs.putBool("radmanual", manualRadar);
}

void ledColor(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(0, 255 - r);
  ledcWrite(1, 255 - g);
  ledcWrite(2, 255 - b);
}

void notifyColor(uint8_t r, uint8_t g, uint8_t b) {
  if (inQuietHours()) return;
  alertR = 0; alertG = g; alertB = b;
  alertUntil = millis() + 7000;
}

Rgb rollingColor() {
  const uint32_t periods[] = {60000,30000,12000};
  float hue=(millis()%periods[gradientSpeed])*6.0f/periods[gradientSpeed];
  return {static_cast<uint8_t>(255*constrain(fabsf(hue-3)-1,0.0f,1.0f)),
          static_cast<uint8_t>(255*constrain(2-fabsf(hue-2),0.0f,1.0f)),
          static_cast<uint8_t>(255*constrain(2-fabsf(hue-4),0.0f,1.0f))};
}

void updateLamp() {
  if (ledTest >= 0) {
    const Rgb test[] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255},
                        {255, 255, 255}, {0, 0, 0}};
    Rgb c = test[ledTest];
    // Bypass LEDC PWM in diagnostic mode to distinguish a PWM fault from
    // a disconnected or failed LED channel. These pins are active low.
    digitalWrite(LED_R, c.r ? LOW : HIGH);
    digitalWrite(LED_G, c.g ? LOW : HIGH);
    digitalWrite(LED_B, c.b ? LOW : HIGH);
    return;
  }
  if (!lampEnabled) { ledColor(0, 0, 0); return; }
  if (static_cast<int32_t>(alertUntil - millis()) > 0) {
    const float pulse = 0.55f + 0.45f * sinf(millis() / 330.0f);
    Rgb base = RGB_PALETTE[tabColor[page] % PALETTE_COUNT];
    if (lampMode) base=rollingColor();
    float blend=min(1.0f,(alertUntil-millis())/1000.0f);
    ledColor((alertR*pulse*blend+base.r*(1-blend))*lampPercent/100,
                (alertG*pulse*blend+base.g*(1-blend))*lampPercent/100,
                (alertB*pulse*blend+base.b*(1-blend))*lampPercent/100);
  } else {
    // Case RGB LED. The LCD backlight is on its own GPIO.
    Rgb c = RGB_PALETTE[tabColor[page] % PALETTE_COUNT];
    if (lampMode) c=rollingColor();
    ledColor(static_cast<uint16_t>(c.r) * lampPercent / 100,
             static_cast<uint16_t>(c.g) * lampPercent / 100,
             static_cast<uint16_t>(c.b) * lampPercent / 100);
  }
}

void startLedTest() {
  ledcDetachPin(LED_R); ledcDetachPin(LED_G); ledcDetachPin(LED_B);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);
  ledTest = 0;
}

void stopLedTest() {
  digitalWrite(LED_R, HIGH); digitalWrite(LED_G, HIGH); digitalWrite(LED_B, HIGH);
  ledcWrite(0, 255); ledcWrite(1, 255); ledcWrite(2, 255);
  ledcAttachPin(LED_R, 0); ledcAttachPin(LED_G, 1); ledcAttachPin(LED_B, 2);
  ledTest = -1;
  redTestUntil = 0;
}

void text(int x, int y, const String &s, uint16_t color = WHITE, int font = 2) {
  tft.setTextColor(color, BG);
  tft.drawString(s, x, y, font);
}

void updateText(int x, int y, int width, int height, const String &s,
                uint16_t color = WHITE, int font = 2) {
  tft.fillRect(x, y, width, height, BG);
  text(x, y, s, color, font);
}

void bar(int x, int y, int width, float score, uint16_t color) {
  tft.fillRoundRect(x, y, width, 10, 5, 0x2948);
  int fill = constrain(static_cast<int>(width * score / 100.0f), 0, width);
  if (fill) tft.fillRoundRect(x, y, fill, 10, 5, color);
}

void button(int x, int y, int w, int h, const String &label,
            uint16_t fill = PANEL, uint16_t ink = WHITE);
void frame(const String &title);

void recordTrend() {
  myTrend[trendNext] = constrain(static_cast<int>(roundf(myScore)), 0, 100);
  regionalTrend[trendNext] = constrain(static_cast<int>(roundf(regionalScore)), 0, 100);
  openingTrend[trendNext] = opening ? 1 : 0;
  trendNext = (trendNext + 1) % TREND_SAMPLES;
  if (trendCount < TREND_SAMPLES) ++trendCount;
}

void drawTrend() {
  const int x = 23, y = trendExpanded ? 73 : 196, w = 432, h = trendExpanded ? 169 : 47;
  tft.fillRect(x, y, w, h, BG);
  tft.drawFastHLine(x, y + h - 1, w, 0x2948);
  tft.drawFastHLine(x, y + h / 2, w, 0x2948);
  if (trendCount < 2) return;
  int oldest = (trendNext + TREND_SAMPLES - trendCount) % TREND_SAMPLES;
  for (int i = 1; i < trendCount; ++i) {
    int a = (oldest + i - 1) % TREND_SAMPLES;
    int b = (oldest + i) % TREND_SAMPLES;
    int xa = x + (i - 1) * (w - 1) / (TREND_SAMPLES - 1);
    int xb = x + i * (w - 1) / (TREND_SAMPLES - 1);
    tft.drawLine(xa, y + h - 1 - myTrend[a] * (h - 2) / 100,
                 xb, y + h - 1 - myTrend[b] * (h - 2) / 100, CYAN);
    tft.drawLine(xa, y + h - 1 - regionalTrend[a] * (h - 2) / 100,
                 xb, y + h - 1 - regionalTrend[b] * (h - 2) / 100, GREEN);
    if (openingTrend[b]) tft.drawFastVLine(xb, y + h - 9, 9, AMBER);
  }
}

void drawExpandedTrend() {
  frame("PROPAGATION HISTORY");
  text(23, 47, "15 MINUTES  |  CYAN: MY  GREEN: REGION", MUTED);
  drawTrend();
  int peakMy = 0, peakRegion = 0;
  for (int i = 0; i < trendCount; ++i) {
    peakMy = max(peakMy, static_cast<int>(myTrend[i]));
    peakRegion = max(peakRegion, static_cast<int>(regionalTrend[i]));
  }
  text(23, 249, "PEAK " + String(peakMy) + " / " + String(peakRegion) + "   AMBER: OPENING", WHITE);
}

void drawBanner(bool force = true) {
  String clock=time(nullptr)>=1600000000 ? localStamp(time(nullptr)) : "SYNCING...";
  bool hold=WiFi.status()!=WL_CONNECTED || millis()-clockSyncAt>86400000;
  static String previous;
  String key=clock+String(WiFi.status())+String(loginStage)+String(alertClient.connected())+String(clockSynced)+String(hold)+String(batteryPercent);
  if(!force && key==previous) return;
  previous=key;
  tft.fillRect(0,0,W,41,PANEL); tft.setTextColor(WHITE,PANEL);
  tft.drawString("HAM DESK",13,13,2);
  tft.drawCentreString(clock,240,7,4);
  tft.fillCircle(348,12,3,WiFi.status()==WL_CONNECTED?GREEN:AMBER);
  tft.fillCircle(361,12,3,alertClient.connected() && loginStage==4?CYAN:MUTED);
  tft.setTextColor(clockSynced && !hold?GREEN:AMBER,PANEL); tft.drawString(clockSynced?(hold?"HOLD":"NTP"):"WAIT",341,22,1);
  uint16_t ink=!batteryValid?MUTED:batteryPercent<=10?RED:batteryPercent<=25?AMBER:GREEN;
  tft.drawRoundRect(382,12,24,13,2,ink); tft.fillRect(406,16,3,5,ink);
  if(batteryValid) {
    int width=batteryPercent*20/100;
    if(width) tft.fillRect(384,14,width,9,ink);
  }
  tft.setTextColor(ink,PANEL);
  tft.drawRightString(batteryValid?"~"+String(batteryPercent)+"%":"--",470,10,2);
  tft.drawFastHLine(0,40,W,0x2948);
}
void frame(const String &title) {
  tft.fillScreen(BG); drawBanner();
  // A subtle content boundary and five distinct navigation buttons.
  tft.drawRoundRect(10,45,460,229,9,0x2948);
  tft.fillRect(0,277,W,43,PANEL);
  const char *tabs[]={"PROP","SPOTS","RADAR","HEALTH","SETUP"};
  for(int i=0;i<5;++i) {
    int x=i*96; uint16_t fill=i==page?CYAN:BG;
    tft.fillRoundRect(x+4,282,88,33,7,fill);
    tft.setTextColor(i==page?BG:MUTED,fill); tft.drawCentreString(tabs[i],x+48,290,2);
  }
  if(unreadSpot) tft.fillCircle(174,291,5,AMBER);
}

void drawProp() {
  if (trendExpanded) { drawExpandedTrend(); return; }
  frame("APRS PROPVIEW");
  if (propUrl.isEmpty()) {
    text(24, 75, "Set PropView URL in SETUP", AMBER, 4);
    return;
  }
  if (!propLive) {
    text(24, 75, "Waiting for PropView", AMBER, 4);
    text(24, 112, propUrl.substring(0, 48), MUTED);
    return;
  }
  tft.drawRoundRect(16,49,217,111,8,0x2948);
  tft.drawRoundRect(246,49,217,111,8,0x2948);
  text(22, 56, "MY STATION", MUTED);
  text(22, 81, String(myScore, 0), CYAN, 4);
  text(94, 91, myLevel, CYAN);
  bar(22, 120, 204, myScore, CYAN);
  text(22, 143, String(directCount) + " direct stations / 1h", MUTED);
  text(255, 56, "REGIONAL", MUTED);
  text(255, 81, String(regionalScore, 0), GREEN, 4);
  text(328, 91, regionalLevel, GREEN);
  bar(255, 120, 204, regionalScore, GREEN);
  text(255, 143, String(regionalCount) + " RF stations / 1h", MUTED);
  text(22, 165, "RF " + String(rfConnected ? "LIVE" : "OFF"), rfConnected ? GREEN : RED);
  text(145, 165, "IS " + String(aprsConnected ? "LIVE" : "OFF"), aprsConnected ? GREEN : RED);
  text(255, 165, eventState == "normal" ? "15 MIN TREND" : "OPENING " + eventState, eventState == "normal" ? MUTED : AMBER);
  if (!trendExpanded) drawTrend();
  text(22, 250, station + "  " + String(trendCount * 15 / 60) + "m history", MUTED);
}

void updatePropChanged(const PropSnapshot &before) {
  if (before.myScore != myScore) {
    updateText(22, 81, 70, 34, String(myScore, 0), CYAN, 4);
    bar(22, 120, 204, myScore, CYAN);
  }
  if (before.myLevel != myLevel) updateText(94, 91, 130, 20, myLevel, CYAN);
  if (before.directCount != directCount)
    updateText(22, 143, 204, 20, String(directCount) + " direct stations / 1h", MUTED);
  if (before.regionalScore != regionalScore) {
    updateText(255, 81, 71, 34, String(regionalScore, 0), GREEN, 4);
    bar(255, 120, 204, regionalScore, GREEN);
  }
  if (before.regionalLevel != regionalLevel) updateText(328, 91, 130, 20, regionalLevel, GREEN);
  if (before.regionalCount != regionalCount)
    updateText(255, 143, 204, 20, String(regionalCount) + " RF stations / 1h", MUTED);
  if (before.rf != rfConnected)
    updateText(22, 165, 115, 20, "RF " + String(rfConnected ? "LIVE" : "OFF"), rfConnected ? GREEN : RED);
  if (before.aprs != aprsConnected)
    updateText(145, 165, 105, 20, "IS " + String(aprsConnected ? "LIVE" : "OFF"), aprsConnected ? GREEN : RED);
  if (trendExpanded) { drawExpandedTrend(); return; }
  drawTrend();
  updateText(22, 250, 220, 20, station + "  " + String(trendCount * 15 / 60) + "m history", MUTED);
  if (before.eventState != eventState)
    updateText(255, 165, 204, 20,
               eventState == "normal" ? "15 MIN TREND" : "OPENING " + eventState,
               eventState == "normal" ? MUTED : AMBER);
}

bool aircraftPoint(const Aircraft &a, int &x, int &y) {
  float angle = a.bearing * DEG_TO_RAD;
  float east = sinf(angle) * a.distance;
  float north = cosf(angle) * a.distance;
  // Project only briefly between reports; old feed data must not wander forever.
  float seconds = min((millis() - lastRadarSuccess) / 1000.0f, 60.0f);
  if (a.track >= 0 && a.speed > 0) {
    float traveled = a.speed * seconds / 3600.0f;
    east += sinf(a.track * DEG_TO_RAD) * traveled;
    north += cosf(a.track * DEG_TO_RAD) * traveled;
  }
  if (east * east + north * north > radarRadiusNm * radarRadiusNm) return false;
  float scale = 102.0f / radarRadiusNm;
  x = 145 + static_cast<int>(roundf(east * scale));
  y = 161 - static_cast<int>(roundf(north * scale));
  return true;
}

void drawRadarGrid() {
  radarCanvas.drawCircle(120, 119, 102, 0x2948);
  radarCanvas.drawCircle(120, 119, 51, 0x2948);
  radarCanvas.drawFastHLine(18, 119, 204, 0x2948);
  radarCanvas.drawFastVLine(120, 17, 204, 0x2948);
  radarCanvas.fillCircle(120, 119, 3, AMBER);
  radarCanvas.setTextColor(MUTED, BG);
  radarCanvas.drawString("N", 114, 1, 2);
  radarCanvas.drawString("E", 227, 111, 2);
  radarCanvas.drawString("S", 114, 209, 2);
  radarCanvas.drawString("W", 2, 111, 2);
}

int trailSlot(const char *hex) {
  for (int i = 0; i < MAX_AIRCRAFT; ++i)
    if (trailHex[i][0] && strcmp(trailHex[i], hex) == 0) return i;
  int slot = 0;
  for (int i = 1; i < MAX_AIRCRAFT; ++i)
    if (!trailHex[i][0] || trails[i][0].when < trails[slot][0].when) slot = i;
  snprintf(trailHex[slot], sizeof(trailHex[slot]), "%s", hex);
  trailCount[slot] = 0;
  return slot;
}

void recordAircraftTrails() {
  for (int i = 0; i < aircraftCount; ++i) {
    int slot = trailSlot(aircraft[i].hex);
    for (int n = 3; n > 0; --n) trails[slot][n] = trails[slot][n - 1];
    trails[slot][0] = {aircraft[i].distance, aircraft[i].bearing, millis()};
    trailCount[slot] = min(static_cast<int>(trailCount[slot]) + 1, 4);
  }
}

void drawRadarTrails() {
  for (int i = 0; i < aircraftCount; ++i) {
    int slot = trailSlot(aircraft[i].hex);
    for (int n = trailCount[slot] - 1; n > 0; --n) {
      TrailPoint a = trails[slot][n], b = trails[slot][n - 1];
      if (millis() - a.when > 180000) continue;
      Aircraft first = aircraft[i], second = first;
      first.distance = a.distance; first.bearing = a.bearing; first.track = -1;
      second.distance = b.distance; second.bearing = b.bearing; second.track = -1;
      int x1, y1, x2, y2;
      if (aircraftPoint(first, x1, y1) && aircraftPoint(second, x2, y2))
        radarCanvas.drawLine(x1 - RADAR_X, y1 - RADAR_Y, x2 - RADAR_X, y2 - RADAR_Y,
                             n >= 2 ? MUTED : (aircraft[i].altitude >= 10000 ? CYAN : GREEN));
    }
  }
}

void drawRadarMarks() {
  for (int i = 0; i < aircraftCount; ++i) {
    int x, y;
    if (!aircraftPoint(aircraft[i], x, y)) continue;
    bool selected = selectedAircraft == aircraft[i].hex;
    uint16_t color = selected ? AMBER : aircraft[i].altitude >= 10000 ? CYAN : GREEN;
    radarCanvas.fillCircle(x - RADAR_X, y - RADAR_Y, selected ? 5 : 4, color);
    if (aircraft[i].track >= 0) {
      float angle = aircraft[i].track * DEG_TO_RAD;
      radarCanvas.drawLine(x - RADAR_X, y - RADAR_Y,
                           x - RADAR_X + static_cast<int>(sinf(angle) * 9),
                           y - RADAR_Y - static_cast<int>(cosf(angle) * 9), color);
    }
  }
}

void drawRadarSweep() {
  // One revolution in twelve seconds; drawing one line needs no framebuffer.
  float angle = (millis() % 12000UL) * (TWO_PI / 12000.0f);
  radarCanvas.drawLine(120, 119, 120 + static_cast<int>(sinf(angle) * 101),
                       119 - static_cast<int>(cosf(angle) * 101), 0x2DAA);
}

void drawRadarCanvas() {
  if (!radarCanvasReady) return;
  radarCanvas.fillSprite(BG);
  drawRadarGrid();
  drawRadarTrails();
  drawRadarSweep();
  drawRadarMarks();
  radarCanvas.pushSprite(RADAR_X, RADAR_Y);
}

void drawRadarPanel() {
  tft.fillRect(277, 49, 190, 222, BG);
  text(281, 53, radarState, radarState == "LIVE" ? GREEN : AMBER);
  text(281, 77, String(aircraftCount) + " aircraft", WHITE, 4);
  button(281, 117, 34, 34, "-");
  text(326, 125, String(radarRadiusNm) + " NM", CYAN);
  button(414, 117, 34, 34, "+");
  int picked = -1;
  for (int i = 0; i < aircraftCount; ++i)
    if (selectedAircraft == aircraft[i].hex) { picked = i; break; }
  if (picked >= 0) {
    const Aircraft &a = aircraft[picked];
    text(281, 165, a.flight, WHITE, 4);
    text(281, 198, String(a.distance, 1) + " NM  " + String(a.bearing, 0) + " deg", CYAN);
    text(281, 222, a.altitude < 0 ? "Ground" : String(a.altitude) + " ft", WHITE);
    text(281, 243, String(a.speed) + " kt", MUTED);
  } else text(281, 174, "Tap an aircraft", MUTED);
  text(281, 260, manualRadar ? "ADSB.lol / MANUAL" : "ADSB.lol / PROPVIEW", MUTED);
}

void drawRadar() {
  frame("LOCAL AIRCRAFT");
  drawRadarCanvas();
  drawRadarPanel();
}

void updateRadar() {
  if (page != 2 || spotlightOpen) return;
  drawRadarCanvas();
  drawRadarPanel();
}

void pollRadar() {
  bool locationReady = manualRadar ? manualLatSet && manualLonSet : stationLocationReady;
  if (!locationReady || WiFi.status() != WL_CONNECTED) {
    radarState = locationReady ? "WI-FI OFFLINE" : "NO LOCATION";
    if (page == 2) drawRadarPanel();
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();  // Public, read-only aircraft feed; no credentials sent.
  HTTPClient http;
  http.setConnectTimeout(1200); http.setTimeout(2200);
  http.setUserAgent("HamDesk-CYD/1.0");
  http.useHTTP10(true);
  String url = "https://api.adsb.lol/v2/point/" +
               String(manualRadar ? manualLat : stationLat, 5) + "/" +
               String(manualRadar ? manualLon : stationLon, 5) + "/" + String(radarRadiusNm);
  if (!http.begin(client, url)) { radarState = "FEED ERROR"; drawRadarPanel(); return; }
  int code = http.GET();
  if (code != 200) {
    radarState = code == 429 ? "RATE LIMITED" : "FEED ERROR " + String(code);
    if (code == 429) nextRadar = millis() + 180000;
    http.end();
    drawRadarPanel();
    Serial.printf("Radar HTTP %d\n", code);
    return;
  }
  StaticJsonDocument<512> filter;
  for (auto key : {"hex", "flight", "alt_baro", "gs", "track", "dst", "dir", "seen_pos"})
    filter["ac"][0][key] = true;
  DynamicJsonDocument doc(32768);
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    radarState = "PARSE ERROR";
    drawRadarPanel();
    Serial.printf("Radar parse: %s\n", error.c_str());
    return;
  }
  oldAircraftCount = aircraftCount;
  memcpy(oldAircraft, aircraft, sizeof(aircraft));
  aircraftCount = 0;
  for (JsonObject item : doc["ac"].as<JsonArray>()) {
    if (aircraftCount >= MAX_AIRCRAFT) break;
    if (item["dst"].isNull() || item["dir"].isNull() || (item["seen_pos"] | 0.0f) > 60) continue;
    Aircraft &a = aircraft[aircraftCount++];
    snprintf(a.hex, sizeof(a.hex), "%s", item["hex"] | "");
    const char *flight = item["flight"] | "";
    snprintf(a.flight, sizeof(a.flight), "%s", *flight ? flight : a.hex);
    a.distance = item["dst"] | 0.0f;
    a.bearing = item["dir"] | 0.0f;
    a.track = item["track"].isNull() ? -1 : item["track"].as<float>();
    a.altitude = item["alt_baro"].is<int>() ? item["alt_baro"].as<int>() : -1;
    a.speed = static_cast<int>(item["gs"] | 0.0f);
  }
  bool stillSelected = false;
  for (int i = 0; i < aircraftCount; ++i)
    if (selectedAircraft == aircraft[i].hex) stillSelected = true;
  if (!stillSelected) {
    int closest = -1;
    for (int i = 0; i < aircraftCount; ++i)
      if (closest < 0 || aircraft[i].distance < aircraft[closest].distance) closest = i;
    selectedAircraft = closest < 0 ? "" : String(aircraft[closest].hex);
  }
  radarState = "LIVE";
  lastRadarSuccess = millis();
  recordAircraftTrails();
  updateRadar();
  Serial.printf("Radar: %u aircraft, free heap %u\n", aircraftCount, ESP.getFreeHeap());
}

int spotBand(const Spot &s) {
  float f=s.freq.toFloat(); if(f>1000) f/=1000;
  if(f>=1.8 && f<2) return 1; if(f>=3.5 && f<4) return 2;
  if(f>=7 && f<7.3) return 3; if(f>=10.1 && f<10.15) return 4;
  if(f>=14 && f<14.35) return 5; if(f>=18.068 && f<18.168) return 6;
  if(f>=21 && f<21.45) return 7; if(f>=24.89 && f<24.99) return 8;
  if(f>=28 && f<29.7) return 9; if(f>=50 && f<54) return 10; return 11;
}
const char *BANDS[]={"ALL","160m","80m","40m","30m","20m","17m","15m","12m","10m","6m","OTHER"};
const char *MODES[]={"ALL","CW","SSB","DIGITAL"};
bool spotMatches(const Spot &s) {
  if(bandFilter && spotBand(s)!=bandFilter) return false;
  String mode=s.mode; mode.toUpperCase();
  if(modeFilter==1) return mode=="CW";
  if(modeFilter==2) return mode=="SSB" || mode=="USB" || mode=="LSB";
  if(modeFilter==3) return mode!="CW" && mode!="SSB" && mode!="USB" && mode!="LSB" && !mode.isEmpty();
  return true;
}
void drawSpots() {
  frame("HAMALERT");
  if(selectedSpot>=0 && selectedSpot<spotCount) {
    const Spot &s=spots[selectedSpot]; button(22,51,74,30,"BACK");
    text(112,51,s.call,WHITE,4); text(22,91,s.freq+"  "+s.mode,CYAN,4);
    text(22,126,"SPOT "+(s.spotEpoch?localStamp(s.spotEpoch,true):String("Time unavailable")),WHITE);
    text(22,149,"RX   "+localStamp(s.receivedEpoch,true),MUTED);
    text(22,173,s.location.isEmpty()?"Location not provided":s.location.substring(0,52),WHITE);
    text(22,198,"SPOTTER "+s.spotter.substring(0,32),MUTED);
    text(22,224,s.comment.substring(0,55),WHITE);
    text(22,246,s.comment.substring(55,105),MUTED); return;
  }
  button(22,50,137,30,String("BAND ")+BANDS[bandFilter]);
  button(171,50,137,30,String("MODE ")+MODES[modeFilter]);
  button(320,50,137,30,"PAGE "+String(spotPage+1));
  visibleCount=0; int matched=0;
  for(int i=0;i<spotCount;++i) if(spotMatches(spots[i])) {
    if(matched>=spotPage*5 && visibleCount<5) visibleSpots[visibleCount++]=i; ++matched;
  }
  if(!visibleCount && spotPage) {spotPage=0; drawSpots(); return;}
  if(!visibleCount) text(22,113,hamUser.isEmpty()?"Set HamAlert login in SETUP":"Waiting for matching spots",MUTED,4);
  for(int i=0;i<visibleCount;++i) {
    const Spot &s=spots[visibleSpots[i]]; int y=85+i*33;
    tft.fillRoundRect(18,y,444,30,6,PANEL); tft.setTextColor(WHITE,PANEL);
    tft.drawString(s.call.substring(0,12),25,y+6,2);
    tft.setTextColor(CYAN,PANEL); tft.drawString((s.freq+" "+s.mode).substring(0,23),163,y+6,2);
    tft.setTextColor(MUTED,PANEL); tft.drawRightString(s.spotEpoch?localStamp(s.spotEpoch):"--",453,y+6,2);
  }
  text(22,253,String(matched)+" spots  "+(alertClient.connected() && loginStage==4?"CONNECTED":"RECONNECTING"),MUTED);
}

void drawSpotlight() {
  if (!spotCount) return;
  const Spot &s = spots[0];
  tft.fillScreen(BG);
  drawBanner();
  tft.drawRoundRect(12,48,456,222,12,CYAN);
  text(24, 64, s.call.substring(0, 18), WHITE, 4);
  text(24, 115, (s.freq + "  " + s.mode).substring(0, 30), CYAN, 4);
  text(24, 163, s.location.substring(0, 50), WHITE);
  text(24, 191, s.comment.substring(0, 52), MUTED);
  text(24,218,"SPOT "+(s.spotEpoch?localStamp(s.spotEpoch):String("Time unavailable")),MUTED);
  text(24, 248, "TAP TO OPEN  |  AUTO CLOSES", AMBER);
}

void refreshHealth() {
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  bool propOk = wifiOk && propLive && lastPropSuccess && millis() - lastPropSuccess < 30000;
  updateText(160, 52, 298, 22, wifiOk ? "CONNECTED" : "OFFLINE", wifiOk ? GREEN : RED);
  updateText(160, 82, 298, 22, wifiOk ?
       WiFi.localIP().toString() + "  " + String(WiFi.RSSI()) + " dBm" : "--", WHITE);
  updateText(160, 112, 298, 22, propUrl.isEmpty() ? "NOT SET" : propOk ? "LIVE" : "WAITING",
       propOk ? GREEN : AMBER);
  updateText(160, 142, 298, 22, lastPropSuccess ? String((millis() - lastPropSuccess) / 1000) + " sec ago" : "Never", WHITE);
  updateText(160, 172, 298, 22, hamUser.isEmpty() ? "NOT SET" : alertClient.connected() ? "CONNECTED" : "WAITING",
       alertClient.connected() ? GREEN : AMBER);
  updateText(160,202,298,22,batteryValid?String(batteryMv/1000.0f,3)+" V  /  ~"+String(batteryPercent)+"%":"Reading unavailable",
             batteryValid && batteryPercent<=10?RED:WHITE);
  updateText(22,227,436,20,spaceWeather.substring(0,52),CYAN);
  updateText(22,249,436,20,lastSpace ? spaceStamp.substring(0,24)+"  "+String((millis()-lastSpace)/60000)+"m ago"+(millis()-lastSpace>1800000?" STALE":"") : "NOAA SWPC / waiting for feed",MUTED);
  lastHealthRefresh = millis();
}

void drawHealth() {
  frame("CONNECTION HEALTH");
  text(22, 52, "WI-FI", MUTED);
  text(22, 82, "IP / SIGNAL", MUTED);
  text(22, 112, "PROPVIEW", MUTED);
  text(22, 142, "LAST GOOD", MUTED);
  text(22, 172, "HAMALERT", MUTED);
  text(22, 202, "BATTERY", MUTED);
  text(22,227,spaceWeather.substring(0,52),CYAN);
  text(22,249,lastSpace ? spaceStamp.substring(0,24)+"  "+String((millis()-lastSpace)/60000)+"m ago" : "NOAA SWPC / waiting for feed",MUTED);
  refreshHealth();
}

void drawWifiScan() {
  frame("WI-FI NETWORKS");
  button(22, 51, 74, 32, "BACK");
  button(300, 51, 156, 32, "SCAN AGAIN", CYAN, BG);
  if (!scanCount) text(22, 105, scanInProgress ? "Scanning nearby networks..." :
                       scanFailed ? "Scan failed. Tap SCAN AGAIN." :
                       "No networks found. Tap SCAN AGAIN.", scanInProgress ? CYAN : AMBER);
  for (int i = 0; i < scanCount; ++i) {
    int y = 91 + i * 36;
    button(22, y, 434, 32, scanSsids[i].substring(0, 25) +
           "  " + String(scanRssi[i]) + " dBm" + (scanSecured[i] ? "  LOCK" : "  OPEN"));
  }
}

void scanWifi() {
  if (scanInProgress || scanPressLocked) return;
  scanPressLocked = true;
  wifiScanView = true;
  scanCount = 0;
  scanInProgress = true;
  scanStartedAt = millis();
  scanFailed = false;
  drawWifiScan();
  if (apMode) WiFi.mode(WIFI_AP_STA);
  WiFi.scanDelete();
  int started = WiFi.scanNetworks(true, false, false, 120);
  if (started == WIFI_SCAN_FAILED) {
    scanInProgress = false; scanFailed = true; drawWifiScan();
    Serial.println("Wi-Fi scan failed to start");
  } else Serial.println("Wi-Fi scan started");
}

void finishWifiScan() {
  if (!scanInProgress) return;
  int found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) {
    if (millis() - scanStartedAt < 8000) return;
    found = WIFI_SCAN_FAILED;
    Serial.println("Wi-Fi scan timed out");
  }
  scanInProgress = false;
  scanFailed = found == WIFI_SCAN_FAILED;
  if (found > 0) {
    for (int i = 0; i < found && scanCount < 5; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) continue;
      bool duplicate = false;
      for (int j = 0; j < scanCount; ++j) if (scanSsids[j] == ssid) duplicate = true;
      if (duplicate) continue;
      scanSsids[scanCount] = ssid;
      scanRssi[scanCount] = WiFi.RSSI(i);
      scanSecured[scanCount] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      ++scanCount;
    }
  }
  WiFi.scanDelete();
  Serial.printf("Wi-Fi scan finished: %u networks shown\n", scanCount);
  if (!apMode && WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    nextWifiRetry = millis() + 30000;
  }
  if (page == 4 && wifiScanView && !keyboardOpen) drawWifiScan();
}

void button(int x, int y, int w, int h, const String &label, uint16_t fill,
            uint16_t ink) {
  tft.fillRoundRect(x, y, w, h, 7, fill);
  tft.drawRoundRect(x, y, w, h, 7, fill == CYAN ? WHITE : 0x2948);
  tft.setTextColor(ink, fill);
  tft.drawCentreString(label, x + w / 2, y + (h - 16) / 2, 2);
}

void settingRow(int y, const String &label, const String &value) {
  text(22, y, label, MUTED);
  text(22, y + 19, value.substring(0, 46), WHITE);
  tft.drawFastHLine(22, y + 43, 436, 0x2948);
}

void drawSetup() {
  if (wifiScanView) { drawWifiScan(); return; }
  frame("SETTINGS");
  if (settingsView == 0) {
    const char *items[] = {"WI-FI", "PROPVIEW", "HAMALERT", "CASE LIGHT", "ALERTS", "DEVICE", "DISPLAY", "RADAR", "CLOCK"};
    for (int i = 0; i < 9; ++i)
      button(22 + (i % 3) * 149, 51 + (i / 3) * 72, 137, 60, items[i]);
    return;
  }
  button(22, 51, 74, 32, "BACK");
  if (settingsView == 1) {
    text(115, 57, "WI-FI", CYAN, 4);
    settingRow(87, "Network name (tap to edit)", wifiSsid.isEmpty() ? "Not set" : wifiSsid);
    settingRow(137, "Password (tap to edit)", wifiPassword.isEmpty() ? "Not set" : "********");
    button(22, 192, 204, 34, "SCAN NETWORKS", CYAN, BG);
    button(240, 192, 216, 34, wifiConnecting ? "CONNECTING..." : "CONNECT NOW", CYAN, BG);
    button(22, 235, 204, 33, "START SETUP AP");
  } else if (settingsView == 2) {
    text(115, 57, "PROPVIEW", CYAN, 4);
    settingRow(103, "Server URL (tap to edit)", propUrl.isEmpty() ? "Not set" : propUrl);
    text(22, 179, propLive ? "Connected" : "Waiting for server", propLive ? GREEN : AMBER);
    text(22, 216, "Local LAN address or relay URL", MUTED);
  } else if (settingsView == 3) {
    text(115, 57, "HAMALERT", CYAN, 4);
    settingRow(89, "Username", hamUser.isEmpty() ? "Not set" : hamUser);
    settingRow(134, "Password", hamPassword.isEmpty() ? "Not set" : "********");
    settingRow(179, "Server host", hamHost);
    settingRow(224, "Port", String(hamPort));
  } else if (settingsView == 4) {
    text(115, 57, "CASE LIGHT", CYAN, 4);
    button(22,91,137,33,lampMode?"GRADIENT":"STATIC",CYAN,BG);
    const char *speeds[]={"SLOW","MEDIUM","FAST"};
    button(171,91,137,33,speeds[gradientSpeed]);
    const char *names[]={"PROP","SPOTS","RADAR","HEALTH","SETUP"};
    button(320,91,137,33,names[lightTab]);
    for (int i = 0; i < PALETTE_COUNT; ++i) {
      Rgb c = RGB_PALETTE[i];
      uint16_t color = tft.color565(c.r, c.g, c.b);
      int x = 27 + (i % 6) * 74, y = 137 + (i / 6) * 39;
      tft.fillRoundRect(x, y, 54, 28, 5, color);
      if (tabColor[lightTab] == i) tft.drawRoundRect(x - 3, y - 3, 60, 34, 6, WHITE);
    }
    text(22, 209, "BRIGHTNESS", MUTED);
    text(248,209,RGB_NAMES[tabColor[lightTab]],WHITE);
    for (int i = 0; i < 5; ++i)
      button(22 + i * 89, 227, 82, 28, String(i * 25) + "%",
             lampPercent == i * 25 ? CYAN : PANEL, lampPercent == i * 25 ? BG : WHITE);
    button(22, 258, 120, 18, lampEnabled ? "LIGHT ON" : "LIGHT OFF");
    button(157, 258, 148, 18, "TEST CHANNELS");
  } else if (settingsView == 5) {
    text(115, 57, "LED ALERTS", CYAN, 4);
    button(22, 105, 300, 34, propAlerts ? "PROP OPENING: ON" : "PROP OPENING: OFF");
    button(22, 150, 300, 34, spotAlerts ? "HAMALERT: ON" : "HAMALERT: OFF");
    button(22, 195, 300, 34, meshAlerts ? "MESH: ON" : "MESH: OFF");
    text(337, 111, "Score " + String(openingScore), WHITE);
    button(335, 149, 48, 34, "-"); button(398, 149, 48, 34, "+");
    button(22,238,137,32,String("POP ")+spotlightSeconds+"s");
    button(171,238,137,32,quietHours?"QUIET ON":"QUIET OFF");
    button(320,238,137,32,"QUIET HOURS");
  } else if (settingsView == 6) {
    text(115, 57, "DEVICE", CYAN, 4);
    text(22, 101, apMode ? "Setup AP: " + WiFi.softAPSSID() : "IP: " + WiFi.localIP().toString(), WHITE);
    text(22, 124, "AP / web password: " + setupPassword, MUTED);
    button(22, 151, 204, 34, "CHANGE PASSWORD");
    button(240, 151, 216, 34, "CALIBRATE TOUCH");
    button(22, 201, 204, 39, "START SETUP AP");
  } else if (settingsView == 9) {
    text(115, 57, "RADAR CENTER", CYAN, 4);
    settingRow(91, "Latitude (-90 to 90)", manualLatSet ? String(manualLat, 5) : "Not set");
    settingRow(141, "Longitude (-180 to 180)", manualLonSet ? String(manualLon, 5) : "Not set");
    button(22, 200, 204, 38, manualRadar ? "MANUAL: ACTIVE" : "USE MANUAL");
    button(240, 200, 216, 38, manualRadar ? "USE PROPVIEW" : "PROPVIEW: ACTIVE");
    text(22, 253, manualLatSet && manualLonSet ? "Wi-Fi required for aircraft feed" : "Enter both coordinates first", MUTED);
  } else if (settingsView == 11) {
    text(115,57,"QUIET HOURS",CYAN,4);
    text(22,105,"Suppress LED alerts and spotlight",MUTED);
    button(22,146,208,45,"START "+String(quietStart)+":00");
    button(248,146,208,45,"END "+String(quietEnd)+":00");
    text(22,218,"Tap to advance hour. Uses local time.",MUTED);
    text(22,244,"Equal start/end means quiet all day.",MUTED);
  } else if (settingsView == 10) {
    text(115,57,"LOCAL TIME",CYAN,4);
    button(22,91,436,36,String(ZONES[zonePreset]));
    text(22,138,offsetLabel()+"  (fixed mode)",WHITE);
    button(22,166,96,34,"-1 HR"); button(130,166,96,34,"+1 HR");
    button(240,166,102,34,"-15 MIN"); button(354,166,102,34,"+15 MIN");
    button(22,213,208,35,"SYNC NIST NOW");
    text(248,220,clockSynced?"NTP synchronized":"Waiting for NIST",clockSynced?GREEN:AMBER);
    text(22,253,zonePreset?"US preset: automatic daylight saving":"Fixed offset: adjust for daylight saving",MUTED);
  } else if (settingsView == 8) {
    text(115, 57, "DISPLAY", CYAN, 4);
    const char *names[] = {"GREEN", "AMBER", "CYAN"};
    for (int i = 0; i < 3; ++i)
      button(22 + i * 149, 94, 137, 38, names[i], theme == i ? CYAN : PANEL,
             theme == i ? BG : WHITE);
    button(22, 144, 208, 37, nightMode ? "NIGHT: ON" : "NIGHT: OFF");
    button(248, 144, 208, 37, autoNight ? "22-06: AUTO" : "AUTO: OFF");
    button(22, 191, 208, 37, idleCycle ? "IDLE CYCLE: ON" : "IDLE CYCLE: OFF");
    text(248, 192, "TIME: SETUP > CLOCK", MUTED);
    button(248, 216, 208, 30, "CLOCK SETTINGS");
    text(22, 251, "Night dim: " + String(nightBacklight) + "/255", MUTED);
    button(248, 250, 60, 26, "DIM-"); button(322, 250, 60, 26, "DIM+");
  } else {
    text(115, 57, "LED CHANNEL TEST", CYAN, 4);
    text(22, 99, "Tap one channel and check the rear LED", MUTED);
    const char *labels[] = {"RED", "GREEN", "BLUE", "WHITE", "OFF"};
    for (int i = 0; i < 5; ++i)
      button(22 + (i % 3) * 149, 145 + (i / 3) * 62, 137, 48, labels[i],
             ledTest == i ? CYAN : PANEL, ledTest == i ? BG : WHITE);
  }
}

void draw() {
  if (spotlightOpen) { drawSpotlight(); return; }
  if (page == 0) drawProp();
  else if (page == 1) drawSpots();
  else if (page == 2) drawRadar();
  else if (page == 3) drawHealth();
  else drawSetup();
}

void startAp();
void connectWifi();

const char *fieldName(uint8_t field) {
  const char *names[] = {"", "Wi-Fi network", "Wi-Fi password", "PropView URL",
                         "HamAlert username", "HamAlert password", "HamAlert host", "HamAlert port",
                         "Setup AP / web password", "Radar latitude", "Radar longitude"};
  return field < 11 ? names[field] : "";
}

const char *keyboardRow(int row) {
  static const char *rows[4][3] = {
    {"qwertyuiop", "asdfghjkl", "zxcvbnm./:"},
    {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM./:"},
    {"1234567890", "-_/@:;()$&", ".?!#%*+=,'"},
    {"[]{}<>|\\~`", "\"^", ""}
  };
  return rows[keyboardMode % 4][row];
}

void drawEditValue() {
  tft.fillRect(17, 70, 446, 29, BG);
  String visible = editValue;
  if (editField == 2 || editField == 5 || editField == 8) {
    visible = "";
    for (size_t i = 0; i < editValue.length(); ++i) visible += '*';
  }
  if (visible.length() > 40) visible = "..." + visible.substring(visible.length() - 37);
  text(20, 73, visible + "_", WHITE);
}

void drawKeyboard() {
  tft.fillScreen(BG);
  text(20, 15, fieldName(editField), CYAN, 4);
  text(20, 49, editorError.isEmpty() ? "Tap keys, then SAVE" : editorError, editorError.isEmpty() ? MUTED : AMBER);
  drawEditValue();
  for (int row = 0; row < 3; ++row) {
    String keys = keyboardRow(row);
    int left = 20 + (10 - keys.length()) * 22;
    for (int col = 0; col < keys.length(); ++col)
      button(left + col * 44, 108 + row * 38, 40, 33, String(keys[col]));
  }
  button(20, 225, 68, 34, keyboardMode >= 2 ? "abc" : "123");
  button(93, 225, 68, 34, keyboardMode >= 2 ? "MORE" : keyboardMode == 1 ? "abc" : "SHIFT");
  button(166, 225, 166, 34, "SPACE");
  button(337, 225, 123, 34, "DELETE");
  button(20, 271, 210, 39, "CANCEL");
  button(250, 271, 210, 39, "SAVE", CYAN, BG);
}

void startEditor(uint8_t field) {
  editField = field;
  keyboardMode = field == 9 || field == 10 ? 2 : 0;
  editorError = "";
  switch (field) {
    case 1: editValue = wifiSsid; break;
    case 2: editValue = wifiPassword; break;
    case 3: editValue = propUrl; break;
    case 4: editValue = hamUser; break;
    case 5: editValue = hamPassword; break;
    case 6: editValue = hamHost; break;
    case 7: editValue = String(hamPort); break;
    case 8: editValue = ""; break;
    case 9: editValue = manualLatSet ? String(manualLat, 5) : ""; break;
    case 10: editValue = manualLonSet ? String(manualLon, 5) : ""; break;
    default: editValue = "";
  }
  keyboardOpen = true;
  drawKeyboard();
}

void saveEditor() {
  if (editField != 1 && editField != 2 && editField != 5 && editField != 8) editValue.trim();
  if (editField == 8 && (editValue.length() < 8 || editValue.length() > 63)) {
    editorError = "Password must be 8-63 characters"; drawKeyboard(); return;
  }
  float coordinate = 0;
  if ((editField == 9 || editField == 10) &&
      !validCoordinate(editValue, editField == 9 ? 90 : 180, coordinate)) {
    editorError = editField == 9 ? "Enter latitude -90 to 90" : "Enter longitude -180 to 180";
    drawKeyboard(); return;
  }
  switch (editField) {
    case 1: wifiSsid = editValue; break;
    case 2: wifiPassword = editValue; break;
    case 3:
      propUrl = editValue;
      if (!propUrl.isEmpty() && !propUrl.startsWith("http://")) propUrl = "http://" + propUrl;
      while (propUrl.endsWith("/")) propUrl.remove(propUrl.length() - 1);
      propLive = false; trendCount = trendNext = 0;
      nextProp = millis() + 1000;
      break;
    case 4: hamUser = editValue; alertClient.stop(); break;
    case 5: hamPassword = editValue; alertClient.stop(); break;
    case 6: hamHost = editValue; alertClient.stop(); break;
    case 7: hamPort = constrain(editValue.toInt(), 1, 65535); alertClient.stop(); break;
    case 8: setupPassword = editValue; break;
    case 9: manualLat = coordinate; manualLatSet = true; if (manualRadar) resetRadarCenter(); break;
    case 10: manualLon = coordinate; manualLonSet = true; if (manualRadar) resetRadarCenter(); break;
  }
  saveSettings();
  keyboardOpen = false;
  if (editField == 8 && apMode) {
    WiFi.softAPdisconnect(true);
    String suffix = WiFi.macAddress(); suffix.replace(":", "");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("HamDesk-" + suffix.substring(suffix.length() - 4), setupPassword.c_str());
  }
  if (editField == 2 && scanSelectionPending) {
    scanSelectionPending = false;
    connectWifi();
  }
  drawSetup();
}

void handleSettingsTouch(uint16_t x, uint16_t y) {
  if (wifiScanView) {
    if (x >= 22 && x < 96 && y >= 51 && y < 83) {
      wifiScanView = false; scanPressLocked = false; drawSetup();
    }
    else if (x >= 300 && y >= 51 && y < 83) scanWifi();
    else if (x >= 22 && x < 456 && y >= 91 && y < 91 + scanCount * 36) {
      int selected = (y - 91) / 36;
      wifiSsid = scanSsids[selected]; wifiPassword = "";
      wifiScanView = false; scanPressLocked = false; scanSelectionPending = true;
      startEditor(2);
    }
    return;
  }
  if (settingsView == 0) {
    if (y >= 51 && y < 255 && x >= 22 && x < 457) {
      int col = (x - 22) / 149, row = (y - 51) / 72;
      int choice = row * 3 + col;
      if (choice < 9) { settingsView = choice == 8 ? 10 : choice == 6 ? 8 : choice == 7 ? 9 : choice + 1; drawSetup(); }
    }
    return;
  }
  if (x >= 22 && x < 96 && y >= 51 && y < 83) {
    if (settingsView == 7) { stopLedTest(); settingsView = 4; }
    else settingsView = 0;
    drawSetup(); return;
  }
  if (settingsView == 1) {
    if (y >= 87 && y < 131) startEditor(1);
    else if (y >= 137 && y < 181) startEditor(2);
    else if (y >= 192 && y < 227 && x < 230) scanWifi();
    else if (y >= 192 && y < 227 && x >= 240) { connectWifi(); drawSetup(); }
    else if (y >= 235 && y < 269 && x < 230) startAp();
  } else if (settingsView == 2) {
    if (y >= 103 && y < 147) startEditor(3);
  } else if (settingsView == 3) {
    if (y >= 89 && y < 134) startEditor(4);
    else if (y >= 134 && y < 179) startEditor(5);
    else if (y >= 179 && y < 224) startEditor(6);
    else if (y >= 224 && y < 268) startEditor(7);
  } else if (settingsView == 4) {
    if (y >= 91 && y < 125 && x >= 22 && x < 467) {
      if(x<159) lampMode=!lampMode; else if(x<308) gradientSpeed=(gradientSpeed+1)%3; else lightTab=(lightTab+1)%5; saveSettings(); drawSetup();
    } else if (y >= 134 && y < 205 && x >= 24 && x < 467) {
      int col = (x - 24) / 74, row = (y - 134) / 39;
      int choice = row * 6 + col;
      if (choice < PALETTE_COUNT) { tabColor[lightTab] = choice; saveSettings(); drawSetup(); }
    } else if (y >= 227 && y < 256 && x >= 22 && x < 459) {
      int choice = (x - 22) / 89;
      lampPercent = min(choice, 4) * 25;
      saveSettings(); drawSetup();
    } else if (y >= 258 && y < 277 && x < 145) {
      lampEnabled = !lampEnabled; saveSettings(); drawSetup();
    } else if (y >= 258 && y < 277 && x >= 157 && x < 305) {
      startLedTest(); settingsView = 7; drawSetup();
    }
  } else if (settingsView == 5) {
    if (y >= 105 && y < 139 && x < 325) propAlerts = !propAlerts;
    else if (y >= 150 && y < 184 && x < 325) spotAlerts = !spotAlerts;
    else if (y >= 195 && y < 229 && x < 325) meshAlerts = !meshAlerts;
    else if (y >= 149 && y < 184 && x >= 335 && x < 386) openingScore = max(0, openingScore - 5);
    else if (y >= 149 && y < 184 && x >= 398) openingScore = min(100, openingScore + 5);
    else if(y>=238 && y<270) {
      if(x<159) spotlightSeconds=spotlightSeconds>=20?0:spotlightSeconds+4;
      else if(x<308) quietHours=!quietHours; else {settingsView=11;}
    }
    saveSettings(); drawSetup();
  } else if (settingsView == 6) {
    if (y >= 151 && y < 186 && x < 230) startEditor(8);
    else if (y >= 151 && y < 186 && x >= 240) { prefs.putBool("calnext", true); ESP.restart(); }
    else if (y >= 201 && y < 241) startAp();
  } else if (settingsView == 9) {
    if (y >= 91 && y < 135) startEditor(9);
    else if (y >= 141 && y < 185) startEditor(10);
    else if (y >= 200 && y < 239 && x >= 22 && x < 226 && manualLatSet && manualLonSet) {
      manualRadar = true; resetRadarCenter(); saveSettings(); drawSetup();
    } else if (y >= 200 && y < 239 && x >= 240 && x < 456) {
      manualRadar = false; resetRadarCenter(); saveSettings(); drawSetup();
    }
  } else if (settingsView == 11) {
    if(y>=146 && y<191) {if(x<230) quietStart=(quietStart+1)%24; else quietEnd=(quietEnd+1)%24;}
    saveSettings(); drawSetup();
  } else if (settingsView == 10) {
    if(y>=91 && y<127) zonePreset=(zonePreset+1)%5;
    else if(y>=166 && y<200) {
      zonePreset=0; utcMinutes=constrain(utcMinutes+(x<118?-60:x<226?60:x<342?-15:15),-720,840);
    } else if(y>=213 && y<248 && x<230) { clockSynced=false; sntp_stop(); configTime(0,0,"time.nist.gov","pool.ntp.org"); }
    configureZone(); saveSettings(); drawSetup();
  } else if (settingsView == 8) {
    if (y >= 94 && y < 133 && x < 459) theme = min(static_cast<int>((x - 22) / 149), 2);
    else if (y >= 144 && y < 182 && x < 230) { nightMode = !nightMode; autoNight = false; }
    else if (y >= 144 && y < 182 && x >= 248) { autoNight = !autoNight; updateNightSchedule(); }
    else if (y >= 191 && y < 229 && x < 230) idleCycle = !idleCycle;
    else if (y >= 216 && y < 250 && x >= 248) settingsView = 10;
    else if (y >= 250 && x >= 248 && x < 309) nightBacklight = max(5, static_cast<int>(nightBacklight) - 10);
    else if (y >= 250 && x >= 322 && x < 383) nightBacklight = min(150, static_cast<int>(nightBacklight) + 10);
    if (autoNight) updateNightSchedule();
    applyTheme(); saveSettings(); drawSetup();
  } else if (settingsView == 7 && y >= 145 && y < 255 && x >= 22 && x < 459) {
    int choice = ((y - 145) / 62) * 3 + (x - 22) / 149;
    if (choice < 5) { ledTest = choice; drawSetup(); }
  }
}

void handleKeyboardTouch(uint16_t x, uint16_t y) {
  if (y >= 108 && y < 217) {
    int row = (y - 108) / 38;
    String keys = keyboardRow(row);
    int left = 20 + (10 - keys.length()) * 22;
    int col = (static_cast<int>(x) - left) / 44;
    if (x >= left && col >= 0 && col < keys.length() && (x - left) % 44 < 40 && editValue.length() < 120) {
      editValue += keys[col]; drawEditValue();
    }
  } else if (y >= 225 && y < 260) {
    if (x < 89) { keyboardMode = keyboardMode >= 2 ? 0 : 2; drawKeyboard(); }
    else if (x < 163) { keyboardMode = keyboardMode >= 2 ? (keyboardMode == 2 ? 3 : 2) :
                                      (keyboardMode == 1 ? 0 : 1); drawKeyboard(); }
    else if (x < 335 && editValue.length() < 120) { editValue += ' '; drawEditValue(); }
    else if (x >= 337 && editValue.length()) { editValue.remove(editValue.length() - 1); drawEditValue(); }
  } else if (y >= 271) {
    if (x < 235) {
      keyboardOpen = false;
      if (scanSelectionPending) {
        scanSelectionPending = false;
        wifiSsid = prefs.getString("ssid", "");
        wifiPassword = prefs.getString("wifi", "");
      }
      drawSetup();
    }
    else if (x >= 250) saveEditor();
  }
}

void handleRadarTouch(uint16_t x, uint16_t y) {
  if (x >= 281 && y >= 117 && y < 152) {
    uint8_t radius = radarRadiusNm;
    if (x < 316) radius = radius == 100 ? 50 : 25;
    else if (x >= 414 && x < 449) radius = radius == 25 ? 50 : 100;
    if (radius != radarRadiusNm) {
      radarRadiusNm = radius;
      prefs.putUChar("radarrad", radius);
      oldAircraftCount = aircraftCount;
      memcpy(oldAircraft, aircraft, sizeof(aircraft));
      aircraftCount = 0;
      radarState = "UPDATING";
      drawRadar();
      nextRadar = millis();
    }
    return;
  }
  if (x > 257 || y < 52 || y > 267) return;
  int closest = -1, best = 144;
  for (int i = 0; i < aircraftCount; ++i) {
    int px, py;
    if (!aircraftPoint(aircraft[i], px, py)) continue;
    int distanceSquared = (static_cast<int>(x) - px) * (static_cast<int>(x) - px) +
                          (static_cast<int>(y) - py) * (static_cast<int>(y) - py);
    if (distanceSquared < best) { closest = i; best = distanceSquared; }
  }
  if (closest < 0) return;
  oldAircraftCount = aircraftCount;
  memcpy(oldAircraft, aircraft, sizeof(aircraft));
  selectedAircraft = aircraft[closest].hex;
  updateRadar();
}

bool gtRead(uint16_t reg, uint8_t *data, size_t count) {
  Wire.beginTransmission(0x5D);
  Wire.write(reg >> 8); Wire.write(reg & 255);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(0x5D, count) != count) return false;
  for (size_t i = 0; i < count; ++i) data[i] = Wire.read();
  return true;
}

void gtClear() {
  Wire.beginTransmission(0x5D);
  Wire.write(0x81); Wire.write(0x4E); Wire.write(0);
  Wire.endTransmission();
}

bool getTouch(uint16_t &x, uint16_t &y) {
  if (gt911) {
    uint8_t status = 0, p[4];
    if (!gtRead(0x814E, &status, 1) || !(status & 0x80)) return false;
    bool found = (status & 0x0F) > 0 && gtRead(0x8150, p, sizeof(p));
    gtClear();
    if (!found) return false;
    uint16_t rawX = p[0] | (p[1] << 8), rawY = p[2] | (p[3] << 8);
    x = constrain(rawY, 0, W - 1);
    y = constrain(319 - static_cast<int>(rawX), 0, H - 1);
    return true;
  }
  return touchReady && tft.getTouch(&x, &y, 150);
}

void setupTouch() {
  Wire.begin(33, 32);
  Wire.setClock(100000);
  Wire.beginTransmission(0x5D);
  gt911 = Wire.endTransmission() == 0;
  if (gt911) {
    Serial.println("Touch: GT911 capacitive");
    touchReady = true;
    return;
  }
  // On the resistive board GPIO33 is XPT2046 chip select. The failed GT911
  // probe temporarily repurposed it as I2C SDA, so release I2C and restore CS.
  Wire.end();
  pinMode(33, OUTPUT);
  digitalWrite(33, HIGH);
  Serial.println("Touch: XPT2046 resistive");
  // Board starting calibration with the Y direction corrected for this panel;
  // the web setup can request precise four-corner calibration.
  uint16_t calibration[5] = {200, 3559, 194, 3741, 7};
  if (prefs.getBytesLength("touch") == sizeof(calibration)) {
    prefs.getBytes("touch", calibration, sizeof(calibration));
  }
  if (prefs.getBool("calnext", false)) {
    prefs.putBool("calnext", false);
    tft.fillScreen(BG);
    tft.setTextColor(WHITE, BG);
    tft.drawCentreString("Touch the calibration targets", W / 2, H / 2 - 20, 2);
    tft.calibrateTouch(calibration, CYAN, BG, 15);
    prefs.putBytes("touch", calibration, sizeof(calibration));
  }
  tft.setTouch(calibration);
  touchReady = true;
}

void startAp() {
  wifiConnecting = false;
  wifiScanView = false;
  spotlightOpen = false; idleActive = false;
  alertClient.stop();
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  String suffix = WiFi.macAddress(); suffix.replace(":", "");
  bool started = WiFi.softAP("HamDesk-" + suffix.substring(suffix.length() - 4), setupPassword.c_str());
  apMode = true;
  page = 4;
  Serial.printf("Setup AP: %s  started=%d  IP=%s\n",
                WiFi.softAPSSID().c_str(), started, WiFi.softAPIP().toString().c_str());
  draw();
}

void connectWifi() {
  if (wifiSsid.isEmpty()) { startAp(); return; }
  if (apMode) WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  apMode = false;
  wifiConnecting = true;
  wifiConnectStarted = millis();
  Serial.println("Wi-Fi connection started");
}

void serviceWifiConnection() {
  if (!wifiConnecting) return;
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
  sntp_set_time_sync_notification_cb(onTimeSync);
    configTime(0, 0, "time.nist.gov", "pool.ntp.org"); configureZone();
    Serial.printf("Wi-Fi IP: %s\n", WiFi.localIP().toString().c_str());
    if (page == 4 && settingsView == 1 && !keyboardOpen) drawSetup();
  } else if (millis() - wifiConnectStarted >= 10000 && !keyboardOpen && !scanSelectionPending) {
    Serial.println("Wi-Fi connection timed out; starting setup AP");
    startAp();
  }
}

bool authorized() {
  if (web.authenticate("admin", setupPassword.c_str())) return true;
  web.requestAuthentication();
  return false;
}

void setupWeb() {
  web.on("/", HTTP_GET, [] {
    if (!authorized()) return;
    String s = "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><title>Ham Desk</title>";
    s += "<style>body{font:18px system-ui;background:#0b1725;color:#f3f6f9;max-width:550px;margin:24px auto;padding:16px}input,select{box-sizing:border-box;width:100%;padding:12px;margin:5px 0 14px;background:#192b3d;border:1px solid #617b91;color:white;border-radius:8px}button{padding:14px 25px;background:#24bbd1;border:0;border-radius:8px;font-weight:bold}</style>";
    s += "<h1>Ham Desk setup</h1><form method='post' action='/save'>";
    s += "Wi-Fi name<input name='ssid' value='" + htmlEscape(wifiSsid) + "'>";
    s += "Wi-Fi password<input name='wifi' type='password' placeholder='Leave blank to keep current'>";
    s += "Setup AP / web password<input name='setup' type='password' minlength='8' maxlength='63' placeholder='Leave blank to keep current'>";
    s += "PropView URL<input name='prop' value='" + htmlEscape(propUrl) + "' placeholder='http://192.168.1.20:8000'>";
    s += "<h2>Radar center</h2>Latitude<input name='radlat' type='number' step='any' min='-90' max='90' value='" +
         String(manualLatSet ? String(manualLat, 5) : "") + "'>";
    s += "Longitude<input name='radlon' type='number' step='any' min='-180' max='180' value='" +
         String(manualLonSet ? String(manualLon, 5) : "") + "'>";
    s += "Center source<select name='radsource'><option value='prop'" + String(!manualRadar ? " selected" : "") +
         ">PropView</option><option value='manual'" + String(manualRadar ? " selected" : "") + ">Manual coordinates</option></select>";
    s += "HamAlert Telnet user<input name='hamuser' value='" + htmlEscape(hamUser) + "'>";
    s += "HamAlert Telnet password<input name='hampass' type='password' placeholder='Leave blank to keep current'>";
    s += "HamAlert host<input name='hamhost' value='" + htmlEscape(hamHost) + "'>";
    s += "HamAlert port<input name='hamport' type='number' value='" + String(hamPort) + "'>";
    s += "Case light brightness<select name='level'>";
    for (int i = 0; i <= 100; i += 25)
      s += "<option value='" + String(i) + "'" + (lampPercent == i ? " selected" : "") + ">" + String(i) + "%</option>";
    s += "</select>";
    s += "<h2>Clock and effects</h2>Timezone<select name='zone'>";
    for(int i=0;i<5;++i) s += "<option value='"+String(i)+"'"+String(zonePreset==i?" selected":"")+">"+ZONES[i]+"</option>";
    s += "</select>Fixed UTC offset (minutes)<input name='utcmins' type='number' min='-720' max='840' step='15' value='"+String(utcMinutes)+"'>";
    s += "LED effect<select name='lampmode'><option value='0'"+String(!lampMode?" selected":"")+">Static per tab</option><option value='1'"+String(lampMode?" selected":"")+">Full RGB rainbow gradient</option></select>";
    s += "Gradient speed<select name='speed'>";
    const char *speeds[]={"Slow (60s)","Medium (30s)","Fast (12s)"};
    for(int i=0;i<3;++i) s += "<option value='"+String(i)+"'"+String(gradientSpeed==i?" selected":"")+">"+speeds[i]+"</option>";
    s += "</select>Spotlight duration (0 disables)<input name='spotsec' type='number' min='0' max='20' value='"+String(spotlightSeconds)+"'>";
    s += "<label><input name='quiet' type='checkbox' style='width:auto' "+String(quietHours?"checked":"")+"> Quiet hours</label>";
    s += "Start hour<input name='qstart' type='number' min='0' max='23' value='"+String(quietStart)+"'>End hour<input name='qend' type='number' min='0' max='23' value='"+String(quietEnd)+"'>";
    s += "<label><input name='lamp' type='checkbox' style='width:auto' " + String(lampEnabled ? "checked" : "") + "> Enable case light</label><p><button>Save and restart</button></form>";
    s.replace("<p><button>Save and restart</button></form>",
      "<p>Opening score threshold (0-100)<input name='openscore' type='number' min='0' max='100' value='" + String(openingScore) + "'>"
      "<label><input name='propalert' type='checkbox' style='width:auto' " + String(propAlerts ? "checked" : "") + "> PropView opening alerts</label><br>"
      "<label><input name='spotalert' type='checkbox' style='width:auto' " + String(spotAlerts ? "checked" : "") + "> HamAlert spot alerts</label><br>"
      "<label><input name='meshalert' type='checkbox' style='width:auto' " + String(meshAlerts ? "checked" : "") + "> Mesh message alerts</label>"
      "<p><button>Save and restart</button></form>");
    s += "<p>Mesh bridge token: <code>" + meshToken + "</code></p>";
    s += "<form method='post' action='/calibrate'><button>Calibrate touch on next boot</button></form>";
    web.send(200, "text/html", s);
  });
  web.on("/save", HTTP_POST, [] {
    if (!authorized()) return;
    String requestedPassword = web.arg("setup");
    if (requestedPassword.length() && (requestedPassword.length() < 8 || requestedPassword.length() > 63)) {
      web.send(400, "text/plain", "Setup password must be 8-63 characters."); return;
    }
    String latitude = web.arg("radlat"), longitude = web.arg("radlon");
    latitude.trim(); longitude.trim();
    float newLat = 0, newLon = 0;
    bool hasLat = validCoordinate(latitude, 90, newLat);
    bool hasLon = validCoordinate(longitude, 180, newLon);
    if ((!latitude.isEmpty() && !hasLat) || (!longitude.isEmpty() && !hasLon) ||
        (web.arg("radsource") == "manual" && (!hasLat || !hasLon))) {
      web.send(400, "text/plain", "Enter valid radar latitude and longitude before using manual center."); return;
    }
    if (hasLat) { manualLat = newLat; manualLatSet = true; }
    if (hasLon) { manualLon = newLon; manualLonSet = true; }
    manualRadar = web.arg("radsource") == "manual";
    if (requestedPassword.length()) setupPassword = requestedPassword;
    wifiSsid = web.arg("ssid"); wifiSsid.trim();
    if (web.arg("wifi").length()) wifiPassword = web.arg("wifi");
    propUrl = web.arg("prop"); propUrl.trim();
    while (propUrl.endsWith("/")) propUrl.remove(propUrl.length() - 1);
    hamUser = web.arg("hamuser"); hamUser.trim();
    if (web.arg("hampass").length()) hamPassword = web.arg("hampass");
    hamHost = web.arg("hamhost"); hamHost.trim();
    hamPort = constrain(web.arg("hamport").toInt(), 1, 65535);
    lampPercent = constrain(web.arg("level").toInt() / 25 * 25, 0, 100);
    lampEnabled = web.hasArg("lamp");
    if(web.hasArg("zone")) zonePreset=constrain(web.arg("zone").toInt(),0,4);
    if(web.hasArg("utcmins")) utcMinutes=constrain(web.arg("utcmins").toInt(),-720,840);
    if(web.hasArg("lampmode")) lampMode=constrain(web.arg("lampmode").toInt(),0,1);
    if(web.hasArg("speed")) gradientSpeed=constrain(web.arg("speed").toInt(),0,2);
    if(web.hasArg("spotsec")) spotlightSeconds=constrain(web.arg("spotsec").toInt(),0,20);
    if(web.hasArg("qstart")) {quietHours=web.hasArg("quiet"); quietStart=constrain(web.arg("qstart").toInt(),0,23); quietEnd=constrain(web.arg("qend").toInt(),0,23);}
    openingScore = constrain(web.arg("openscore").toInt(), 0, 100);
    propAlerts = web.hasArg("propalert");
    spotAlerts = web.hasArg("spotalert");
    meshAlerts = web.hasArg("meshalert");
    saveSettings();
    web.send(200, "text/plain", "Saved. Ham Desk is restarting.");
    delay(400); ESP.restart();
  });
  web.on("/calibrate", HTTP_POST, [] {
    if (!authorized()) return;
    prefs.putBool("calnext", true);
    web.send(200, "text/plain", "Touch all four targets on the CYD after it restarts.");
    delay(400); ESP.restart();
  });
  web.on("/api/mesh/event", HTTP_POST, [] {
    if (web.header("X-HamDesk-Token") != meshToken) { web.send(403, "text/plain", "Forbidden"); return; }
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, web.arg("plain"))) { web.send(400, "text/plain", "Bad JSON"); return; }
    String from = doc["from"] | "unknown";
    String body = doc["text"] | "";
    String kind = doc["type"] | "message";
    String signal = doc["signal"] | "";
    if (body.isEmpty()) body = "Node update";
    for (int i = 4; i > 0; --i) meshEvents[i] = meshEvents[i - 1];
    meshEvents[0] = {from.substring(0, 25), body.substring(0, 90), kind, signal};
    meshCount = min(static_cast<int>(meshCount) + 1, 5);
    lastMesh = millis();
    if (kind == "message" && meshAlerts) notifyColor(0, 30, 55);
    if (page == 3) refreshHealth();
    web.send(204, "text/plain", "");
  });
  const char *headers[] = {"X-HamDesk-Token"};
  web.collectHeaders(headers, 1);
  web.begin();
}

bool fetchJson(const String &path, JsonDocument &doc, JsonDocument &filter) {
  if (propUrl.isEmpty() || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(1200); http.setTimeout(1800);
  if (!http.begin(propUrl + path)) return false;
  // A local Tailscale relay can require this device-specific token. Direct
  // PropView instances ignore the extra header.
  http.addHeader("X-HamDesk-Token", meshToken);
  int code = http.GET();
  bool ok = code == 200 && deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter)) == DeserializationError::Ok;
  http.end();
  return ok;
}

void pollProp() {
  if (propUrl.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  PropSnapshot before = {propLive, rfConnected, aprsConnected, myScore, regionalScore,
                         directCount, regionalCount, myLevel, regionalLevel, station, eventState};
  StaticJsonDocument<256> statusFilter;
  statusFilter["station"] = true;
  statusFilter["rf_connected"] = true;
  statusFilter["aprs_is_connected"] = true;
  statusFilter["latitude"] = true;
  statusFilter["longitude"] = true;
  StaticJsonDocument<512> status;
  bool a = fetchJson("/api/status", status, statusFilter);
  StaticJsonDocument<256> propFilter;
  for (auto key : {"my_score", "my_level", "my_stations_1h", "score", "level", "regional_stations_1h"}) propFilter[key] = true;
  propFilter["event"]["state"] = true;
  StaticJsonDocument<512> prop;
  bool b = fetchJson("/api/propagation", prop, propFilter);
  propLive = a && b;
  if (!propLive) {
    if (page == 0 && before.live && !spotlightOpen) drawProp();
    return;
  }
  lastPropSuccess = millis();
  station = String(status["station"] | "");
  float lat = status["latitude"] | 0.0f, lon = status["longitude"] | 0.0f;
  if (lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 &&
      (fabsf(lat) > 0.001f || fabsf(lon) > 0.001f)) {
    stationLat = lat; stationLon = lon; stationLocationReady = true;
  }
  rfConnected = status["rf_connected"] | false;
  aprsConnected = status["aprs_is_connected"] | false;
  myScore = prop["my_score"] | 0.0f;
  regionalScore = prop["score"] | 0.0f;
  myLevel = String(prop["my_level"] | "none"); myLevel.toUpperCase();
  regionalLevel = String(prop["level"] | "none"); regionalLevel.toUpperCase();
  directCount = prop["my_stations_1h"] | 0;
  regionalCount = prop["regional_stations_1h"] | 0;
  eventState = String(prop["event"]["state"] | "normal");
  bool nowOpen = eventState == "confirmed" || eventState == "peak" ||
                 myScore >= openingScore || regionalScore >= openingScore;
  if (nowOpen && !opening && propAlerts && before.live) notifyColor(55, 24, 0);
  opening = nowOpen;
  recordTrend();
  if (page == 0 && !spotlightOpen) {
    if (before.live) updatePropChanged(before);
    else drawProp();
  }
}

void processSpot(const String &line) {
  int start = line.indexOf('{');
  if (start < 0) return;
  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, line.substring(start))) return;
  String call = doc["callsign"] | "";
  if (call.isEmpty()) call = String(doc["dx"] | "");
  if (call.isEmpty()) return;
  String freq;
  if (doc["frequency"].is<const char *>()) freq = String(doc["frequency"].as<const char *>());
  else if (doc["frequency"].is<float>() || doc["frequency"].is<int>()) freq = String(doc["frequency"].as<float>(), 3);
  if (freq.isEmpty()) {
    if (doc["freq"].is<const char *>()) freq = String(doc["freq"].as<const char *>());
    else if (doc["freq"].is<float>() || doc["freq"].is<int>()) freq = String(doc["freq"].as<float>(), 3);
  }
  String mode = doc["mode"] | "";
  String spotter = doc["spotter"] | "";
  if (spotter.isEmpty()) spotter = String(doc["de"] | "");
  String location = doc["state"] | "";
  String entity = doc["entity"] | "";
  String grid = doc["grid"] | "";
  if (grid.isEmpty()) grid = String(doc["locator"] | "");
  if (location.isEmpty()) location = entity;
  else if (!entity.isEmpty() && entity != location) location += ", " + entity;
  if (!grid.isEmpty()) location += (location.isEmpty() ? "" : "  ") + grid;
  if (location.isEmpty()) location = String(doc["summitName"] | "");
  if (location.isEmpty()) location = String(doc["parkName"] | "");
  String comment = doc["comment"] | "";
  if (comment.isEmpty()) comment = String(doc["comments"] | "");
  String spotTime = doc["time"] | "";
  if (spotTime.isEmpty()) spotTime = String(doc["timestamp"] | "");
  if(spotTime.isEmpty() && doc["timestamp"].is<int64_t>()) spotTime=doc["timestamp"].as<String>();
  String spotDate=doc["date"] | "";
  if(!spotDate.isEmpty() && spotTime.length()<=9) spotTime=spotDate+"T"+spotTime;
  String receivedUtc = "time unavailable";
  time_t now = time(nullptr);
  if (now > 1600000000) {
    struct tm utc;
    gmtime_r(&now, &utc);
    char timestamp[24];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M UTC", &utc);
    receivedUtc = timestamp;
  }
  time_t parsed = parseSpotTime(spotTime, now);
  for(int i=0;i<spotCount;++i) {
    if(spots[i].call==call && spots[i].freq==freq && spots[i].mode==mode && spots[i].spotter==spotter &&
       ((parsed && spots[i].spotEpoch==parsed) || (!parsed && millis()-spots[i].receivedAt<60000))) return;
  }
  if (selectedSpot >= 0) selectedSpot = selectedSpot >= SPOT_CAPACITY-1 ? -1 : selectedSpot + 1;
  for (int i = SPOT_CAPACITY-1; i > 0; --i) spots[i] = spots[i - 1];
  Spot incoming;
  incoming.call = call; incoming.freq = freq; incoming.mode = mode;
  incoming.spotter = spotter; incoming.location = location;
  incoming.comment = comment; incoming.spotTime = spotTime;
  incoming.receivedUtc = receivedUtc; incoming.receivedAt = millis();
  incoming.spotEpoch = parsed; incoming.receivedEpoch = now;
  spots[0] = incoming;
  spotCount = min(static_cast<int>(spotCount) + 1, SPOT_CAPACITY);
  if (page != 4 && !keyboardOpen && spotlightSeconds && !inQuietHours() && spotMatches(incoming)) {
    spotlightOpen = true;
    spotlightUntil = millis() + spotlightSeconds*1000;
    drawSpotlight();
  }
  if (page != 1) {
    unreadSpot = true;
    tft.fillCircle(174, 291, 6, 0xF9BA);
  }
  if (spotAlerts) notifyColor(45, 0, 32);
  if (page == 1 && !spotlightOpen) draw();
}

void pollHamAlert() {
  if(hamUser.isEmpty() || hamPassword.isEmpty() || WiFi.status()!=WL_CONNECTED) return;
  if(!alertClient.connected()) {
    loginStage=0;
    if(static_cast<int32_t>(millis()-nextAlertConnect)<0) return;
    nextAlertConnect=millis()+10000;
    if(!alertClient.connect(hamHost.c_str(),hamPort,900)) return;
    loginStage=1; loginDeadline=millis()+2500; alertLine="";
  }
  if(loginStage<4) {
    bool prompt=false; int budget=512;
    while(alertClient.available() && budget-->0) {alertClient.read(); prompt=true;}
    if(prompt || static_cast<int32_t>(millis()-loginDeadline)>=0) {
      if(loginStage==1) alertClient.println(hamUser);
      else if(loginStage==2) alertClient.println(hamPassword);
      else {alertClient.println("set/json"); lastAlertKeepalive=millis();}
      ++loginStage; loginDeadline=millis()+2500;
    }
    return;
  }
  int budget=2048;
  while(alertClient.available() && budget-->0) {
    char c=alertClient.read();
    if(c=='\n') {processSpot(alertLine); alertLine="";}
    else if(c!='\r' && alertLine.length()<4096) alertLine+=c;
  }
  if(millis()-lastAlertKeepalive>=120000) {alertClient.println("echo"); lastAlertKeepalive=millis();}
}

void pollSpaceWeather() {
  if(WiFi.status()!=WL_CONNECTED || time(nullptr)<1600000000) return;
  WiFiClientSecure client; client.setCACert(NOAA_CA); client.setHandshakeTimeout(5);
  HTTPClient http; http.setConnectTimeout(1200); http.setTimeout(1800);
  http.useHTTP10(true);
  if(!http.begin(client,"https://services.swpc.noaa.gov/products/noaa-scales.json")) return;
  int response=http.GET();
  if(response==200) {
    StaticJsonDocument<256> filter; filter["0"]["R"]["Scale"]=true;
    filter["0"]["S"]["Scale"]=true; filter["0"]["G"]["Scale"]=true;
    filter["0"]["DateStamp"]=true; filter["0"]["TimeStamp"]=true;
    StaticJsonDocument<512> doc;
    DeserializationError error=deserializeJson(doc,http.getStream(),DeserializationOption::Filter(filter));
    if(!error) {
      JsonObject current=doc["0"]; if(!current.isNull()) {
        spaceWeather="NOAA  R"+String(current["R"]["Scale"]|"?")+"  S"+String(current["S"]["Scale"]|"?")+"  G"+String(current["G"]["Scale"]|"?");
        time_t epoch=parseSpotTime(String(current["DateStamp"]|"")+"T"+String(current["TimeStamp"]|""),time(nullptr));
        spaceStamp=epoch?localStamp(epoch,true):"Time unavailable"; lastSpace=millis();
        Serial.println("NOAA scales received over verified TLS");
      }
    } else Serial.printf("NOAA parse error: %s\n",error.c_str());
  } else Serial.printf("NOAA HTTP result: %d\n",response);
  http.end();
}
// Deliberately limited USB diagnostics: no credentials or service configuration.
void serviceDiagnostics() {
  int budget=64;
  while(Serial.available() && budget-->0) {
    char c=Serial.read();
    if(c=='\n' || c=='\r') {
      if(diagnosticCommand=="battery") {sampleBattery(true); logBattery();}
      else if(diagnosticCommand=="redtest") {
        spotlightOpen=false; keyboardOpen=false;
        startLedTest(); redTestUntil=millis()+45000;
        page=4; settingsView=7; draw(); updateLamp();
        Serial.printf("RED TEST: GPIO22=%d GREEN GPIO16=%d BLUE GPIO17=%d AUDIO GPIO4=%d; 45 seconds\n",
                      digitalRead(LED_R),digitalRead(LED_G),digitalRead(LED_B),digitalRead(AUDIO_EN));
      } else if(diagnosticCommand=="ledstop") {
        if(ledTest>=0) stopLedTest(); settingsView=4; draw();
        Serial.println("LED diagnostic stopped");
      }
      diagnosticCommand="";
    } else if(diagnosticCommand.length()<20) diagnosticCommand+=c;
  }
  if(redTestUntil && static_cast<int32_t>(millis()-redTestUntil)>=0) {
    if(ledTest>=0) stopLedTest();
    if(page==4 && settingsView==7) {settingsView=4; draw();}
    redTestUntil=0; Serial.println("RED TEST ended; normal case light restored");
  }
}

void handleTouch() {
  if (!gt911 && millis() - lastTouchLog >= 500) {
    lastTouchLog = millis();
    uint16_t z = tft.getTouchRawZ();
    if (z > 25) Serial.printf("Touch pressure=%u\n", z);
  }
  uint16_t x, y;
  if (!getTouch(x, y)) { scanPressLocked = false; return; }
  if (millis() - touchTime < 250) return;
  touchTime = millis();
  lastInteraction = millis();
  if (spotlightOpen) {
    spotlightOpen = false; page = 1; selectedSpot = 0; unreadSpot = false; draw(); return;
  }
  if (idleActive) { idleActive = false; page = idlePage; draw(); return; }
  Serial.printf("Touch x=%u y=%u page=%u\n", x, y, page);
  if(!keyboardOpen) tft.drawFastHLine(0,40,W,CYAN);
  if (keyboardOpen) { handleKeyboardTouch(x, y); return; }
  if (page == 0 && trendExpanded && y < 277) { trendExpanded = false; draw(); return; }
  if (page == 0 && y >= 190 && y < 271) { trendExpanded = true; draw(); return; }
  if (y >= 277) {
    if (ledTest >= 0) { stopLedTest(); settingsView = 4; }
    page = min(static_cast<int>(x / 96), 4);
    if (page != 4) wifiScanView = false;
    if (page == 1) unreadSpot = false;
    if (page == 2) nextRadar = millis();
    draw();
  }
  else if (page == 1) {
    if (selectedSpot >= 0) {
      if (x >= 22 && x < 96 && y >= 51 && y < 83) { selectedSpot = -1; drawSpots(); }
    } else if(y>=50 && y<80) {
      if(x<159) {bandFilter=(bandFilter+1)%12; spotPage=0;}
      else if(x<308) {modeFilter=(modeFilter+1)%4; spotPage=0;}
      else spotPage=(spotPage+1)%6; drawSpots();
    } else if (y >= 85 && y < 85 + visibleCount * 33) {
      selectedSpot = visibleSpots[(y-85)/33]; drawSpots();
    }
  }
  else if (page == 2) handleRadarTouch(x, y);
  else if (page == 4) handleSettingsTouch(x, y);
}
}  // namespace

void setup() {
  Serial.begin(115200);
  digitalWrite(AUDIO_EN,HIGH); pinMode(AUDIO_EN,OUTPUT); // Keep unused amplifier off.
  setupBattery();
  ledcSetup(3, 5000, 8); ledcAttachPin(BL, 3);
  for (int i = 0; i < 3; ++i) ledcSetup(i, 5000, 8);
  ledcAttachPin(LED_R, 0); ledcAttachPin(LED_G, 1); ledcAttachPin(LED_B, 2);
  ledColor(0, 0, 0);
  pinMode(BOOT, INPUT_PULLUP);
  prefs.begin("hamdesk", false);
  setupPassword = prefs.getString("setup", "");
  if (setupPassword.isEmpty()) { setupPassword = randomHex(12); prefs.putString("setup", setupPassword); }
  meshToken = prefs.getString("meshkey", "");
  if (meshToken.isEmpty()) { meshToken = randomHex(24); prefs.putString("meshkey", meshToken); }
  wifiSsid = prefs.getString("ssid", ""); wifiPassword = prefs.getString("wifi", "");
  propUrl = prefs.getString("prop", ""); hamUser = prefs.getString("hamuser", "");
  hamPassword = prefs.getString("hampass", ""); hamHost = prefs.getString("hamhost", "hamalert.org");
  hamPort = prefs.getUShort("hamport", 7300);
  lampEnabled = prefs.getBool("lamp", true);
  if (prefs.isKey("lampPct")) lampPercent = prefs.getUChar("lampPct", 25);
  else {
    uint8_t oldLevel = prefs.getUChar("level", 14);
    lampPercent = constrain(((static_cast<int>(oldLevel) * 100 / 80 + 12) / 25) * 25, 0, 100);
  }
  if (prefs.getBytesLength("tabcolor") == sizeof(tabColor))
    prefs.getBytes("tabcolor", tabColor, sizeof(tabColor));
  else if (prefs.getBytesLength("tabcolor") == 4) {
    uint8_t oldColors[4];
    prefs.getBytes("tabcolor", oldColors, sizeof(oldColors));
    tabColor[0] = oldColors[0]; tabColor[1] = oldColors[1];
    tabColor[3] = oldColors[2]; tabColor[4] = oldColors[3];
    prefs.putBytes("tabcolor", tabColor, sizeof(tabColor));
  }
  for (uint8_t &color : tabColor) if (color >= PALETTE_COUNT) color = 0;
  if(prefs.getUChar("palettever",0)<1) {
    // Match saved green/blue choices to their nearest color in the new palette.
    const Rgb old[]={{0,190,45},{0,255,255},{0,255,0},{0,140,0},{0,255,90},{0,90,255},
                     {0,0,255},{0,50,255},{0,170,255},{0,255,160},{0,80,80},{0,0,0}};
    for(uint8_t &color:tabColor) {
      Rgb c=old[color]; int best=INT_MAX; uint8_t selected=0;
      for(int i=0;i<PALETTE_COUNT;++i) {
        Rgb n=RGB_PALETTE[i]; int r=int(c.r)-n.r,g=int(c.g)-n.g,b=int(c.b)-n.b;
        int distance=r*r+g*g+b*b;
        if(distance<best) {best=distance; selected=i;}
      }
      color=selected;
    }
    prefs.putBytes("tabcolor",tabColor,sizeof(tabColor)); prefs.putUChar("palettever",1);
  }
  propAlerts = prefs.getBool("propalert", true);
  spotAlerts = prefs.getBool("spotalert", true);
  meshAlerts = prefs.getBool("meshalert", true);
  openingScore = prefs.getUChar("openscore", 70);
  theme = min(static_cast<int>(prefs.getUChar("theme", 2)), 2);
  nightMode = prefs.getBool("night", false);
  autoNight = prefs.getBool("autonight", false);
  utcMinutes = constrain(prefs.getShort("utcmins", prefs.getChar("utcoffset", 0)*60), -720, 840);
  zonePreset = min(4, int(prefs.getUChar("zone", 0))); configureZone();
  checkTimeFormatting();
  lampMode = min(1, int(prefs.getUChar("lampmode", 0)));
  gradientSpeed = min(2, int(prefs.getUChar("speed", 1)));
  spotlightSeconds = min(20, int(prefs.getUChar("spotsec", 8)));
  quietHours = prefs.getBool("quiet", false);
  quietStart = min(23, int(prefs.getUChar("qstart",22))); quietEnd = min(23,int(prefs.getUChar("qend",6)));
  idleCycle = prefs.getBool("idlecycle", false);
  normalBacklight = prefs.getUChar("daybl", 255);
  nightBacklight = constrain(static_cast<int>(prefs.getUChar("nightbl", 35)), 5, 150);
  applyTheme();
  radarRadiusNm = prefs.getUChar("radarrad", 50);
  if (radarRadiusNm != 25 && radarRadiusNm != 50 && radarRadiusNm != 100) radarRadiusNm = 50;
  manualLatSet = prefs.isKey("radlat"); manualLonSet = prefs.isKey("radlon");
  manualLat = prefs.getFloat("radlat", 0); manualLon = prefs.getFloat("radlon", 0);
  manualLatSet = manualLatSet && isfinite(manualLat) && fabsf(manualLat) <= 90;
  manualLonSet = manualLonSet && isfinite(manualLon) && fabsf(manualLon) <= 180;
  manualRadar = prefs.getBool("radmanual", false) && manualLatSet && manualLonSet;
  tft.init(); tft.setRotation(1); tft.setTextDatum(TL_DATUM);
  radarCanvas.setColorDepth(8);
  radarCanvasReady = radarCanvas.createSprite(RADAR_W, RADAR_H) != nullptr;
  Serial.printf("Radar canvas: %s, free heap %u\n",
                radarCanvasReady ? "ready" : "allocation failed", ESP.getFreeHeap());
  tft.fillScreen(BG);
  text(20, 30, "Ham Desk starting...", WHITE, 4);
  setupTouch();
  connectWifi();
  setupWeb();
  draw();
  nextProp = millis() + 1000;
  nextWifiRetry = millis() + 30000;
  nextAlertConnect = millis() + 1000;
  lastInteraction = millis();
  Serial.println("Ham Desk ready");
}

void loop() {
  sampleBattery();
  serviceDiagnostics();
  web.handleClient();
  if(millis()-lastBanner>=1000 && !keyboardOpen && !spotlightOpen) {
    lastBanner=millis(); drawBanner(false);
    static bool syncReported=false;
    if(clockSynced && !syncReported) {Serial.println("NTP time synchronized"); syncReported=true;}
    if(page==0 && !trendExpanded) updateText(260,250,200,20,lastPropSuccess ? String((millis()-lastPropSuccess)/1000)+"s ago"+(millis()-lastPropSuccess>45000?" STALE":" LIVE") : "NO DATA",MUTED);
  }
  handleTouch();
  finishWifiScan();
  serviceWifiConnection();
  bool wasNight = nightMode;
  updateNightSchedule();
  if (nightMode != wasNight) draw();
  if (spotlightOpen && static_cast<int32_t>(millis() - spotlightUntil) >= 0) {
    spotlightOpen = false; draw();
  }
  if (idleCycle && !apMode && !keyboardOpen && !spotlightOpen && page < 4 &&
      millis() - lastInteraction > 120000) {
    if (!idleActive) { idleActive = true; idlePage = page; lastIdlePage = millis(); }
    else if (millis() - lastIdlePage >= 12000) {
      page = (page + 1) % 3;
      if (page == 2) nextRadar = millis();
      lastIdlePage = millis(); draw();
    }
  }
  updateLamp();
  if(page!=4 && !keyboardOpen && !spotlightOpen && millis()-lastInteraction>2000 && time(nullptr)>=1600000000 && WiFi.status()==WL_CONNECTED && static_cast<int32_t>(millis()-nextSpace)>=0) {
    nextSpace=millis()+600000; pollSpaceWeather(); if(page==3) refreshHealth();
  }
  if (digitalRead(BOOT) == LOW) {
    if (!bootHold) bootHold = millis();
    else if (!bootLong && millis() - bootHold > 5000 && !apMode) {
      bootLong = true;
      startAp();
    }
  } else if (bootHold) {
    if (!bootLong && millis() - bootHold > 50) {
      lastInteraction = millis(); idleActive = false; trendExpanded = false; spotlightOpen = false;
      if (ledTest >= 0) { stopLedTest(); settingsView = 4; }
      page = (page + 1) % 5;
      if (page == 1) unreadSpot = false;
      if (page == 2) nextRadar = millis();
      draw();
    }
    bootHold = 0;
    bootLong = false;
  }
  if (!apMode && page != 4 && !wifiConnecting && !keyboardOpen && !scanSelectionPending &&
      !scanInProgress && !wifiScanView && WiFi.status() != WL_CONNECTED) {
    if (static_cast<int32_t>(millis() - nextWifiRetry) >= 0) {
      nextWifiRetry = millis() + 30000;
      connectWifi(); draw();
    }
  }
  if (!apMode && page != 4 && !keyboardOpen && !scanInProgress && !wifiScanView &&
      static_cast<int32_t>(millis() - nextProp) >= 0) {
    nextProp = millis() + 15000;
    pollProp();
  }
  if (!apMode && page != 4 && !keyboardOpen && !scanInProgress && !wifiScanView) pollHamAlert();
  if (!apMode && page == 2 && !spotlightOpen && static_cast<int32_t>(millis() - nextRadar) >= 0) {
    nextRadar = millis() + 30000;
    pollRadar();
  }
  if (page == 3 && millis() - lastHealthRefresh >= 5000) refreshHealth();
  if (page == 2 && !spotlightOpen && millis() - lastRadarFrame >= 100) {
    lastRadarFrame = millis();
    drawRadarCanvas();
  }
  delay(15);
}
