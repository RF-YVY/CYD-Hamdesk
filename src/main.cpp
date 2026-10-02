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

namespace {
constexpr int W = 480, H = 320;
constexpr int BL = 27, LED_R = 4, LED_G = 16, LED_B = 17, BOOT = 0;
uint16_t BG = 0x0843, PANEL = 0x10A5, WHITE = 0xEF7D;
uint16_t MUTED = 0x8C71, CYAN = 0x45FA, AMBER = 0xFCA1;
uint16_t GREEN = 0x67EC, RED = 0xFA69;
uint8_t theme = 2;
bool nightMode = false, autoNight = false, idleCycle = false;
int8_t utcOffset = 0;
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
bool batterySenseEnabled = false;
float batteryDivider = 2.0f;
uint32_t lastBatteryRead = 0;
int batteryMilliVolts = 0, batteryPercent = -1;
String scanSsids[5];
int32_t scanRssi[5] = {};
bool scanSecured[5] = {};
uint8_t scanCount = 0;
bool wifiScanView = false, scanSelectionPending = false;
bool scanInProgress = false;
struct Rgb { uint8_t r, g, b; };
// This particular board's red LED channel remains dark even with GPIO4 driven
// low directly. Match the on-screen choices to the working green/blue channels.
constexpr Rgb GB_PALETTE[] = {{0, 190, 45}, {0, 255, 255}, {0, 255, 0},
                              {0, 140, 0}, {0, 255, 90}, {0, 90, 255},
                              {0, 0, 255}, {0, 50, 255}, {0, 170, 255},
                              {0, 255, 160}, {0, 80, 80}, {0, 0, 0}};
constexpr int PALETTE_COUNT = sizeof(GB_PALETTE) / sizeof(GB_PALETTE[0]);
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
constexpr int MAX_AIRCRAFT = 32;
struct Aircraft {
  char hex[9], flight[12];
  float distance, bearing, track;
  int altitude, speed;
};
Aircraft aircraft[MAX_AIRCRAFT], oldAircraft[MAX_AIRCRAFT];
uint8_t aircraftCount = 0, oldAircraftCount = 0, radarRadiusNm = 50;
String selectedAircraft, radarState = "WAITING";
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
  uint32_t receivedAt = 0;
};
Spot spots[5];
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
  int hour = ((now / 3600 + utcOffset) % 24 + 24) % 24;
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
  prefs.putBool("batsense", batterySenseEnabled);
  prefs.putFloat("batdiv", batteryDivider);
  prefs.putBytes("tabcolor", tabColor, sizeof(tabColor));
  prefs.putBool("propalert", propAlerts);
  prefs.putBool("spotalert", spotAlerts);
  prefs.putBool("meshalert", meshAlerts);
  prefs.putUChar("openscore", openingScore);
  prefs.putUChar("theme", theme);
  prefs.putBool("night", nightMode);
  prefs.putBool("autonight", autoNight);
  prefs.putChar("utcoffset", utcOffset);
  prefs.putBool("idlecycle", idleCycle);
  prefs.putUChar("daybl", normalBacklight);
  prefs.putUChar("nightbl", nightBacklight);
}

void ledColor(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(0, 255);  // Red emitter is not responding on this unit.
  ledcWrite(1, 255 - g);
  ledcWrite(2, 255 - b);
}

void notifyColor(uint8_t r, uint8_t g, uint8_t b) {
  alertR = r; alertG = g; alertB = b;
  alertUntil = millis() + 7000;
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
    ledColor(alertR * pulse, alertG * pulse, alertB * pulse);
  } else {
    // Case RGB LED. The LCD backlight is on its own GPIO.
    Rgb c = GB_PALETTE[tabColor[page] % PALETTE_COUNT];
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

void drawBatteryBadge() {
  tft.fillRect(187, 8, 100, 25, PANEL);
  tft.setTextColor(batteryPercent >= 0 ? GREEN : MUTED, PANEL);
  tft.drawString(batteryPercent >= 0 ? "BAT " + String(batteryPercent) + "%" : "BAT --", 192, 14, 2);
}

void frame(const String &title) {
  tft.fillScreen(BG);
  tft.fillRect(0, 0, W, 41, PANEL);
  tft.setTextColor(WHITE, PANEL);
  tft.drawString("HAM DESK", 13, 9, 4);
  drawBatteryBadge();
  tft.drawRightString(title, 465, 13, 2);
  tft.fillRect(0, 277, W, 43, PANEL);
  const char *tabs[] = {"PROP", "SPOTS", "RADAR", "HEALTH", "SETUP"};
  for (int i = 0; i < 5; ++i) {
    int x = i * 96;
    if (i == page) tft.fillRect(x + 7, 279, 82, 3, CYAN);
    tft.setTextColor(i == page ? WHITE : MUTED, PANEL);
    tft.drawCentreString(tabs[i], x + 48, 292, 2);
  }
  if (unreadSpot) tft.fillCircle(174, 291, 6, 0xF9BA);
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
  text(281, 260, "Data: ADSB.lol", MUTED);
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
  if (!stationLocationReady || WiFi.status() != WL_CONNECTED) {
    radarState = stationLocationReady ? "WI-FI OFFLINE" : "NO LOCATION";
    if (page == 2) drawRadarPanel();
    return;
  }
  WiFiClientSecure client;
  client.setInsecure();  // Public, read-only aircraft feed; no credentials sent.
  HTTPClient http;
  http.setTimeout(8000);
  http.setUserAgent("HamDesk-CYD/1.0");
  http.useHTTP10(true);
  String url = "https://api.adsb.lol/v2/point/" + String(stationLat, 5) + "/" +
               String(stationLon, 5) + "/" + String(radarRadiusNm);
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

void drawSpots() {
  frame("HAMALERT TELNET");
  if (selectedSpot >= 0 && selectedSpot < spotCount) {
    const Spot &s = spots[selectedSpot];
    button(22, 51, 74, 30, "BACK");
    text(112, 51, s.call, WHITE, 4);
    text(22, 90, s.freq + "  " + s.mode, CYAN, 4);
    text(354, 94, s.spotTime.substring(0, 12), MUTED);
    text(22, 121, "STATION LOCATION", MUTED);
    text(22, 139, s.location.isEmpty() ? "Not provided in this spot" : s.location.substring(0, 52), WHITE);
    text(22, 163, "SPOTTER", MUTED);
    text(22, 181, s.spotter.isEmpty() ? "Not provided" : s.spotter, WHITE);
    text(22, 207, "COMMENT", MUTED);
    text(22, 225, s.comment.isEmpty() ? "No comment" : s.comment.substring(0, 55), WHITE);
    if (s.comment.length() > 55) text(22, 243, s.comment.substring(55, 105), WHITE);
    text(22, 260, "Received " + s.receivedUtc, MUTED);
    return;
  }
  if (hamUser.isEmpty() || hamPassword.isEmpty()) {
    text(24, 67, "Set HamAlert login in SETUP", AMBER, 4);
    return;
  }
  text(22, 50, alertClient.connected() ? "CONNECTED" : "RECONNECTING", alertClient.connected() ? GREEN : AMBER);
  if (!spotCount) text(22, 91, "Waiting for spots...", MUTED, 4);
  for (int i = 0; i < spotCount; ++i) {
    int y = 79 + i * 37;
    text(22, y, spots[i].call, WHITE, 4);
    text(200, y + 6, spots[i].freq + "  " + spots[i].mode, CYAN);
    text(393, y + 6, spots[i].spotTime.substring(0, 5), MUTED);
    tft.drawFastHLine(22, y + 31, 436, 0x2948);
  }
}

void drawSpotlight() {
  if (!spotCount) return;
  const Spot &s = spots[0];
  tft.fillScreen(BG);
  tft.fillRect(0, 0, W, 42, PANEL);
  tft.setTextColor(AMBER, PANEL);
  tft.drawString("NEW HAMALERT SPOT", 18, 10, 4);
  text(24, 64, s.call.substring(0, 18), WHITE, 4);
  text(24, 115, (s.freq + "  " + s.mode).substring(0, 30), CYAN, 4);
  text(24, 163, s.location.substring(0, 50), WHITE);
  text(24, 191, s.comment.substring(0, 52), MUTED);
  text(24, 248, "TAP TO OPEN  |  AUTO CLOSES", AMBER);
}

void updateBattery() {
  if (!batterySenseEnabled || millis() - lastBatteryRead < 15000) return;
  uint32_t sum = 0;
  for (int i = 0; i < 8; ++i) sum += analogReadMilliVolts(35);
  batteryMilliVolts = static_cast<int>(sum / 8 * batteryDivider);
  batteryPercent = batteryMilliVolts < 3000 || batteryMilliVolts > 4350 ? -1 :
                   constrain((batteryMilliVolts - 3300) * 100 / 900, 0, 100);
  lastBatteryRead = millis();
  if (!keyboardOpen && !spotlightOpen) drawBatteryBadge();
}

void refreshHealth() {
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  bool propOk = wifiOk && propLive && lastPropSuccess && millis() - lastPropSuccess < 30000;
  updateText(180, 52, 278, 30, wifiOk ? "CONNECTED" : "OFFLINE", wifiOk ? GREEN : RED, 4);
  updateText(180, 90, 278, 22, wifiOk ?
       WiFi.localIP().toString() + "  " + String(WiFi.RSSI()) + " dBm" : "--", WHITE);
  updateText(180, 124, 278, 30, propUrl.isEmpty() ? "NOT SET" : propOk ? "LIVE" : "WAITING",
       propOk ? GREEN : AMBER, 4);
  updateText(180, 160, 278, 22, lastPropSuccess ? String((millis() - lastPropSuccess) / 1000) + " sec ago" : "Never", WHITE);
  updateText(180, 194, 278, 22, hamUser.isEmpty() ? "NOT SET" : alertClient.connected() ? "CONNECTED" : "WAITING",
       alertClient.connected() ? GREEN : AMBER);
  updateText(180, 227, 278, 30,
             !batterySenseEnabled ? "SENSOR NOT WIRED" : batteryPercent < 0 ? "NO VALID READING" :
             String(batteryPercent) + "%  " + String(batteryMilliVolts / 1000.0f, 2) + " V",
             batteryPercent >= 0 ? GREEN : AMBER, 2);
  lastHealthRefresh = millis();
}

void drawHealth() {
  frame("CONNECTION HEALTH");
  text(22, 52, "WI-FI", MUTED);
  text(22, 90, "IP / SIGNAL", MUTED);
  text(22, 124, "PROPVIEW", MUTED);
  text(22, 160, "LAST GOOD", MUTED);
  text(22, 194, "HAMALERT", MUTED);
  text(22, 227, "BATTERY", MUTED);
  refreshHealth();
}

void drawWifiScan() {
  frame("WI-FI NETWORKS");
  button(22, 51, 74, 32, "BACK");
  button(300, 51, 156, 32, "SCAN AGAIN", CYAN, BG);
  if (!scanCount) text(22, 105, scanInProgress ? "Scanning nearby networks..." :
                       "No networks found. Try scanning again.", scanInProgress ? CYAN : AMBER);
  for (int i = 0; i < scanCount; ++i) {
    int y = 91 + i * 36;
    button(22, y, 434, 32, scanSsids[i].substring(0, 25) +
           "  " + String(scanRssi[i]) + " dBm" + (scanSecured[i] ? "  LOCK" : "  OPEN"));
  }
}

void scanWifi() {
  wifiScanView = true;
  scanCount = 0;
  scanInProgress = true;
  drawWifiScan();
  if (apMode) WiFi.mode(WIFI_AP_STA);
  int found = WiFi.scanNetworks(false, false);
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
  scanInProgress = false;
  drawWifiScan();
}

void button(int x, int y, int w, int h, const String &label, uint16_t fill,
            uint16_t ink) {
  tft.fillRoundRect(x, y, w, h, 7, fill);
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
    const char *items[] = {"WI-FI", "PROPVIEW", "HAMALERT", "CASE LIGHT", "ALERTS", "DEVICE", "DISPLAY"};
    for (int i = 0; i < 7; ++i)
      button(22 + (i % 3) * 149, 51 + (i / 3) * 72, 137, 60, items[i]);
    return;
  }
  button(22, 51, 74, 32, "BACK");
  if (settingsView == 1) {
    text(115, 57, "WI-FI", CYAN, 4);
    settingRow(87, "Network name (tap to edit)", wifiSsid.isEmpty() ? "Not set" : wifiSsid);
    settingRow(137, "Password (tap to edit)", wifiPassword.isEmpty() ? "Not set" : "********");
    button(22, 192, 204, 34, "SCAN NETWORKS", CYAN, BG);
    button(240, 192, 216, 34, "CONNECT NOW", CYAN, BG);
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
    const char *names[] = {"PROP", "SPOTS", "RADAR", "HEALTH", "SETUP"};
    for (int i = 0; i < 5; ++i)
      button(22 + i * 89, 91, 82, 33, names[i], i == lightTab ? CYAN : PANEL,
             i == lightTab ? BG : WHITE);
    for (int i = 0; i < PALETTE_COUNT; ++i) {
      Rgb c = GB_PALETTE[i];
      uint16_t color = tft.color565(c.r, c.g, c.b);
      int x = 27 + (i % 6) * 74, y = 137 + (i / 6) * 39;
      tft.fillRoundRect(x, y, 54, 28, 5, color);
      if (tabColor[lightTab] == i) tft.drawRoundRect(x - 3, y - 3, 60, 34, 6, WHITE);
    }
    text(22, 209, "BRIGHTNESS", MUTED);
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
  } else if (settingsView == 6) {
    text(115, 57, "DEVICE", CYAN, 4);
    text(22, 101, apMode ? "Setup AP: " + WiFi.softAPSSID() : "IP: " + WiFi.localIP().toString(), WHITE);
    text(22, 124, "AP / web password: " + setupPassword, MUTED);
    button(22, 151, 204, 34, "CHANGE PASSWORD");
    button(240, 151, 216, 34, "CALIBRATE TOUCH");
    button(22, 201, 204, 39, "START SETUP AP");
  } else if (settingsView == 8) {
    text(115, 57, "DISPLAY", CYAN, 4);
    const char *names[] = {"GREEN", "AMBER", "CYAN"};
    for (int i = 0; i < 3; ++i)
      button(22 + i * 149, 94, 137, 38, names[i], theme == i ? CYAN : PANEL,
             theme == i ? BG : WHITE);
    button(22, 144, 208, 37, nightMode ? "NIGHT: ON" : "NIGHT: OFF");
    button(248, 144, 208, 37, autoNight ? "22-06: AUTO" : "AUTO: OFF");
    button(22, 191, 208, 37, idleCycle ? "IDLE CYCLE: ON" : "IDLE CYCLE: OFF");
    text(248, 192, "UTC OFFSET " + String(utcOffset), WHITE);
    button(248, 216, 60, 34, "-"); button(322, 216, 60, 34, "+");
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
                         "Setup AP / web password"};
  return field < 9 ? names[field] : "";
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
  keyboardMode = 0;
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
    if (x >= 22 && x < 96 && y >= 51 && y < 83) { wifiScanView = false; drawSetup(); }
    else if (x >= 300 && y >= 51 && y < 83) scanWifi();
    else if (x >= 22 && x < 456 && y >= 91 && y < 91 + scanCount * 36) {
      int selected = (y - 91) / 36;
      wifiSsid = scanSsids[selected]; wifiPassword = "";
      wifiScanView = false; scanSelectionPending = true;
      startEditor(2);
    }
    return;
  }
  if (settingsView == 0) {
    if (y >= 51 && y < 255 && x >= 22 && x < 457) {
      int col = (x - 22) / 149, row = (y - 51) / 72;
      int choice = row * 3 + col;
      if (choice < 7) { settingsView = choice == 6 ? 8 : choice + 1; drawSetup(); }
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
      lightTab = min(static_cast<int>((x - 22) / 89), 4); drawSetup();
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
    saveSettings(); drawSetup();
  } else if (settingsView == 6) {
    if (y >= 151 && y < 186 && x < 230) startEditor(8);
    else if (y >= 151 && y < 186 && x >= 240) { prefs.putBool("calnext", true); ESP.restart(); }
    else if (y >= 201 && y < 241) startAp();
  } else if (settingsView == 8) {
    if (y >= 94 && y < 133 && x < 459) theme = min(static_cast<int>((x - 22) / 149), 2);
    else if (y >= 144 && y < 182 && x < 230) { nightMode = !nightMode; autoNight = false; }
    else if (y >= 144 && y < 182 && x >= 248) { autoNight = !autoNight; updateNightSchedule(); }
    else if (y >= 191 && y < 229 && x < 230) idleCycle = !idleCycle;
    else if (y >= 216 && y < 250 && x >= 248 && x < 309) utcOffset = max(-12, static_cast<int>(utcOffset) - 1);
    else if (y >= 216 && y < 250 && x >= 322 && x < 383) utcOffset = min(14, static_cast<int>(utcOffset) + 1);
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
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; ++i) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    apMode = false;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.printf("Wi-Fi IP: %s\n", WiFi.localIP().toString().c_str());
  } else startAp();
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
    s += "HamAlert Telnet user<input name='hamuser' value='" + htmlEscape(hamUser) + "'>";
    s += "HamAlert Telnet password<input name='hampass' type='password' placeholder='Leave blank to keep current'>";
    s += "HamAlert host<input name='hamhost' value='" + htmlEscape(hamHost) + "'>";
    s += "HamAlert port<input name='hamport' type='number' value='" + String(hamPort) + "'>";
    s += "Case light brightness<select name='level'>";
    for (int i = 0; i <= 100; i += 25)
      s += "<option value='" + String(i) + "'" + (lampPercent == i ? " selected" : "") + ">" + String(i) + "%</option>";
    s += "</select>";
    s += "<label><input name='batsense' type='checkbox' style='width:auto' " + String(batterySenseEnabled ? "checked" : "") + "> Battery voltage sensor on GPIO35 (requires external divider)</label>";
    s += "Battery divider ratio<input name='batdiv' type='number' min='1.1' max='10' step='0.01' value='" + String(batteryDivider, 2) + "'>";
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
    batterySenseEnabled = web.hasArg("batsense");
    batteryDivider = constrain(web.arg("batdiv").toFloat(), 1.1f, 10.0f);
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
  http.setTimeout(6500);
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
  String receivedUtc = "time unavailable";
  time_t now = time(nullptr);
  if (now > 1600000000) {
    struct tm utc;
    gmtime_r(&now, &utc);
    char timestamp[24];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M UTC", &utc);
    receivedUtc = timestamp;
  }
  if (spotTime.isEmpty() && receivedUtc != "time unavailable")
    spotTime = receivedUtc.substring(11, 16) + "Z";
  if (selectedSpot >= 0) selectedSpot = selectedSpot >= 4 ? -1 : selectedSpot + 1;
  for (int i = 4; i > 0; --i) spots[i] = spots[i - 1];
  Spot incoming;
  incoming.call = call; incoming.freq = freq; incoming.mode = mode;
  incoming.spotter = spotter; incoming.location = location;
  incoming.comment = comment; incoming.spotTime = spotTime;
  incoming.receivedUtc = receivedUtc; incoming.receivedAt = millis();
  spots[0] = incoming;
  spotCount = min(static_cast<int>(spotCount) + 1, 5);
  if (page != 4 && !keyboardOpen) {
    spotlightOpen = true;
    spotlightUntil = millis() + 8000;
    drawSpotlight();
  }
  if (page != 1) {
    unreadSpot = true;
    tft.fillCircle(174, 291, 6, 0xF9BA);
  }
  if (spotAlerts) notifyColor(45, 0, 32);
  if (page == 1 && !spotlightOpen) draw();
}

void waitForAlertPrompt() {
  uint32_t started = millis();
  while (alertClient.connected() && millis() - started < 2500) {
    if (alertClient.available()) {
      while (alertClient.available()) alertClient.read();
      return;
    }
    delay(20);
  }
}

void pollHamAlert() {
  if (hamUser.isEmpty() || hamPassword.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  if (!alertClient.connected()) {
    if (static_cast<int32_t>(millis() - nextAlertConnect) < 0) return;
    nextAlertConnect = millis() + 10000;
    if (!alertClient.connect(hamHost.c_str(), hamPort, 3500)) return;
    // Same Telnet negotiation used by HamView on the SenseCAP Indicator.
    waitForAlertPrompt();
    alertClient.println(hamUser);
    waitForAlertPrompt();
    alertClient.println(hamPassword);
    waitForAlertPrompt();
    alertClient.println("set/json");
    alertLine = "";
    lastAlertKeepalive = millis();
    if (page == 1) draw();
  }
  int budget = 2048;
  while (alertClient.available() && budget-- > 0) {
    char c = alertClient.read();
    if (c == '\n') { processSpot(alertLine); alertLine = ""; }
    else if (c != '\r' && alertLine.length() < 4096) alertLine += c;
  }
  if (millis() - lastAlertKeepalive >= 120000) {
    alertClient.println("echo"); lastAlertKeepalive = millis();
  }
}

void handleTouch() {
  if (!gt911 && millis() - lastTouchLog >= 500) {
    lastTouchLog = millis();
    uint16_t z = tft.getTouchRawZ();
    if (z > 25) Serial.printf("Touch pressure=%u\n", z);
  }
  uint16_t x, y;
  if (!getTouch(x, y) || millis() - touchTime < 250) return;
  touchTime = millis();
  lastInteraction = millis();
  if (spotlightOpen) {
    spotlightOpen = false; page = 1; selectedSpot = 0; unreadSpot = false; draw(); return;
  }
  if (idleActive) { idleActive = false; page = idlePage; draw(); return; }
  Serial.printf("Touch x=%u y=%u page=%u\n", x, y, page);
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
    } else if (y >= 79 && y < 79 + spotCount * 37) {
      selectedSpot = (y - 79) / 37;
      drawSpots();
    }
  }
  else if (page == 2) handleRadarTouch(x, y);
  else if (page == 4) handleSettingsTouch(x, y);
}
}  // namespace

void setup() {
  Serial.begin(115200);
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
  batterySenseEnabled = prefs.getBool("batsense", false);
  batteryDivider = prefs.getFloat("batdiv", 2.0f);
  if (batteryDivider < 1.1f || batteryDivider > 10.0f) batteryDivider = 2.0f;
  if (batterySenseEnabled) analogSetPinAttenuation(35, ADC_11db);
  lastBatteryRead = millis() - 15000;
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
  propAlerts = prefs.getBool("propalert", true);
  spotAlerts = prefs.getBool("spotalert", true);
  meshAlerts = prefs.getBool("meshalert", true);
  openingScore = prefs.getUChar("openscore", 70);
  theme = min(static_cast<int>(prefs.getUChar("theme", 2)), 2);
  nightMode = prefs.getBool("night", false);
  autoNight = prefs.getBool("autonight", false);
  utcOffset = constrain(static_cast<int>(prefs.getChar("utcoffset", 0)), -12, 14);
  idleCycle = prefs.getBool("idlecycle", false);
  normalBacklight = prefs.getUChar("daybl", 255);
  nightBacklight = constrain(static_cast<int>(prefs.getUChar("nightbl", 35)), 5, 150);
  applyTheme();
  radarRadiusNm = prefs.getUChar("radarrad", 50);
  if (radarRadiusNm != 25 && radarRadiusNm != 50 && radarRadiusNm != 100) radarRadiusNm = 50;
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
  nextAlertConnect = millis() + 1000;
  lastInteraction = millis();
  Serial.println("Ham Desk ready");
}

void loop() {
  web.handleClient();
  handleTouch();
  updateBattery();
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
  if (!apMode && WiFi.status() != WL_CONNECTED) {
    if (millis() - nextProp > 30000) { nextProp = millis(); connectWifi(); draw(); }
  }
  if (!apMode && static_cast<int32_t>(millis() - nextProp) >= 0) {
    nextProp = millis() + 15000;
    pollProp();
  }
  if (!apMode) pollHamAlert();
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
