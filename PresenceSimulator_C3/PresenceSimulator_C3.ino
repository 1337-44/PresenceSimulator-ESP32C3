/*
 * PresenceSimulator for ESP32-C3 (SuperMini), 2026 adaptation.
 * Based on the idea of SensorsIot/PresenceSimulator by Andreas Spiess (MIT).
 * Copyright (c) 2016 Andreas Spiess (original project)
 * ESP32-C3 adaptation distributed under the MIT license; see LICENSE.
 *
 * Dependencies: Arduino-ESP32 core 3.x or 2.x, Adafruit NeoPixel.
 * Uses no legacy ESP8266 APIs, telemetry, remote firmware updates or API keys.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <ESPmDNS.h>
#include <time.h>
#include <math.h>

// SuperMini pins. Change if your ESP32-C3 board uses different wiring.
static constexpr uint8_t LED_DATA_PIN = 4;      // -> AHCT125 -> WS2812 DIN
static constexpr uint8_t LAMP_DRIVER_PIN = 5;   // logic signal for MOSFET/relay driver
static constexpr uint8_t SETUP_BUTTON_PIN = 3;  // optional button to GND, hold while booting
static constexpr uint16_t MAX_PIXELS = 120;
static constexpr char AP_SSID[] = "PresenceC3-Setup";
static constexpr char AP_PASSWORD[] = "presence-c3-setup"; // CHANGE before deploying
static constexpr char MDNS_NAME[] = "presence-c3";

Preferences prefs;
WebServer server(80);
Adafruit_NeoPixel strip(1, LED_DATA_PIN, NEO_GRB + NEO_KHZ800);

struct Settings {
  String ssid;
  String password;
  String timezone = "CET-1CEST,M3.5.0/2,M10.5.0/3"; // Poland, including DST
  double latitude = 52.23;   // Example only: Warsaw; configure your own location!
  double longitude = 21.01;
  uint16_t pixelCount = 60;
  uint8_t brightness = 72;   // 0..255; intentionally conservative
  uint8_t sunsetLeadMinutes = 60;
  uint8_t stopHour = 22;
};
Settings settings;

enum class ManualMode : uint8_t { Auto, BothOn, BothOff };
enum class SceneMode : uint8_t { TVOnly, LampOnly, Both };
ManualMode manualMode = ManualMode::Auto;
SceneMode sceneMode = SceneMode::Both;
bool tvActive = false, lampActive = false, accessPointMode = false;
uint32_t lastSceneModeChangeMs = 0;
uint32_t sceneModePeriodMs = 0;
uint32_t lastFrameMs = 0;
uint32_t sceneStartMs = 0;
uint32_t sceneFadeMs = 0;
uint32_t sceneHoldMs = 0;
uint8_t fromR = 0, fromG = 0, fromB = 0;
uint8_t toR = 0, toG = 0, toB = 0;
uint8_t currentR = 0, currentG = 0, currentB = 0;
uint32_t lastWiFiAttemptMs = 0;
uint32_t connectionLostSinceMs = 0;
int cachedSunsetDay = 0;
time_t cachedSunsetUtc = 0;

static uint8_t clamp8(int val) { return (uint8_t)constrain(val, 0, 255); }
static String htmlEscape(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;");
  s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}
static bool parseNumber(const String& value, double& result) {
  if (!value.length()) return false;
  char* end = nullptr;
  result = strtod(value.c_str(), &end);
  return (end != value.c_str()) && (*end == '\0') && isfinite(result);
}

void loadSettings() {
  prefs.begin("presence-c3", false);
  settings.ssid = prefs.getString("ssid", "");
  settings.password = prefs.getString("password", "");
  settings.timezone = prefs.getString("timezone", settings.timezone);
  settings.latitude = prefs.getDouble("lat", settings.latitude);
  settings.longitude = prefs.getDouble("lon", settings.longitude);
  settings.pixelCount = constrain((int)prefs.getUShort("leds", settings.pixelCount), 1, (int)MAX_PIXELS);
  settings.brightness = clamp8(prefs.getUChar("bright", settings.brightness));
  settings.sunsetLeadMinutes = constrain((int)prefs.getUChar("lead", settings.sunsetLeadMinutes), 0, 180);
  settings.stopHour = constrain((int)prefs.getUChar("stop", settings.stopHour), 1, 23);
}

void applyLedSettings() {
  strip.updateLength(settings.pixelCount);
  strip.setBrightness(settings.brightness);
  strip.clear();
  strip.show();
}

static double rad(double deg) { return deg * 0.01745329251994329577; }
static double deg(double radians) { return radians * 57.29577951308232; }
static double wrap360(double n) { n = fmod(n, 360.0); return n < 0 ? n + 360.0 : n; }
static double wrap24(double n) { n = fmod(n, 24.0); return n < 0 ? n + 24.0 : n; }

// NOAA-style approximate sunset calculation (zenith=90.833 degrees).
// Returns UTC epoch time for the current *local calendar date* in Central Europe.
// UTC date boundary near the international date line requires extra handling.
time_t calculateSunsetUtc(const tm& localDay, double lat, double lon) {
  const int n = localDay.tm_yday + 1;
  const double lngHour = lon / 15.0;
  const double t = n + ((18.0 - lngHour) / 24.0);
  const double meanAnomaly = 0.9856 * t - 3.289;
  const double trueLongitude = wrap360(meanAnomaly + 1.916 * sin(rad(meanAnomaly))
                                + 0.020 * sin(2.0 * rad(meanAnomaly)) + 282.634);
  double rightAscension = wrap360(deg(atan(0.91764 * tan(rad(trueLongitude)))));
  rightAscension += floor(trueLongitude / 90.0) * 90.0 - floor(rightAscension / 90.0) * 90.0;
  rightAscension /= 15.0;
  const double sinDec = 0.39782 * sin(rad(trueLongitude));
  const double cosDec = cos(asin(sinDec));
  const double cosH = (cos(rad(90.833)) - sinDec * sin(rad(lat))) /
                       (cosDec * cos(rad(lat)));
  if (cosH < -1.0 || cosH > 1.0) return 0; // no sunset on polar day/night
  const double hourAngle = deg(acos(cosH)) / 15.0;
  const double localMean = hourAngle + rightAscension - 0.06571 * t - 6.622;
  const double utcHours = wrap24(localMean - lngHour);
  tm utcMidnight = localDay;
  utcMidnight.tm_hour = 0; utcMidnight.tm_min = 0; utcMidnight.tm_sec = 0;
  utcMidnight.tm_isdst = 0;
  return timegm(&utcMidnight) + (time_t)lround(utcHours * 3600.0);
}

bool getClock(tm& out, time_t& epoch) {
  epoch = time(nullptr);
  if (epoch < 1700000000) return false; // NTP not yet synchronized
  return localtime_r(&epoch, &out) != nullptr;
}

bool isAutomaticWindow() {
  tm today = {};
  time_t now = 0;
  if (!getClock(today, now)) return false;
  const int dayKey = (today.tm_year + 1900) * 10000 + (today.tm_mon + 1) * 100 + today.tm_mday;
  if (dayKey != cachedSunsetDay) {
    cachedSunsetUtc = calculateSunsetUtc(today, settings.latitude, settings.longitude);
    cachedSunsetDay = dayKey;
  }
  return cachedSunsetUtc > 0 && now >= cachedSunsetUtc - settings.sunsetLeadMinutes * 60
         && today.tm_hour < settings.stopHour;
}

void nextSceneColors() {
  // Palette: cooler TV scenes with occasional warm interiors and bright cuts.
  static const uint8_t palette[][3] = {
    {32, 52, 105}, {45, 75, 125}, {108, 96, 66}, {50, 82, 74},
    {112, 64, 51}, {80, 86, 125}, {34, 48, 67}, {128, 109, 92},
    {72, 104, 143}, {138, 126, 100}, {58, 39, 72}, {95, 115, 112}
  };
  fromR = currentR; fromG = currentG; fromB = currentB;
  const uint8_t* target = palette[random((long)(sizeof(palette) / sizeof(palette[0])))];
  const int sceneLevel = random(55, 150);
  toR = clamp8((int)target[0] * sceneLevel / 100);
  toG = clamp8((int)target[1] * sceneLevel / 100);
  toB = clamp8((int)target[2] * sceneLevel / 100);
  const uint32_t duration = (uint32_t)random(250, 2500);
  sceneFadeMs = (random(10) < 3) ? 0 : (uint32_t)random((long)duration);
  sceneHoldMs = duration - sceneFadeMs;
  sceneStartMs = millis();
}

void renderTV() {
  const uint32_t now = millis();
  if ((uint32_t)(now - lastFrameMs) < 35) return;
  lastFrameMs = now;
  const uint32_t age = now - sceneStartMs;
  if (age >= sceneFadeMs + sceneHoldMs) nextSceneColors();
  const uint32_t fadeAge = millis() - sceneStartMs;
  const uint8_t amount = (sceneFadeMs == 0 || fadeAge >= sceneFadeMs)
                         ? 255 : (uint8_t)((fadeAge * 255UL) / sceneFadeMs);
  currentR = (uint8_t)(((uint32_t)fromR * (255 - amount) + (uint32_t)toR * amount) / 255);
  currentG = (uint8_t)(((uint32_t)fromG * (255 - amount) + (uint32_t)toG * amount) / 255);
  currentB = (uint8_t)(((uint32_t)fromB * (255 - amount) + (uint32_t)toB * amount) / 255);
  for (uint16_t i = 0; i < settings.pixelCount; ++i) {
    // Slight spatial variation instead of a perfectly uniform panel.
    const int variance = 88 + ((i * 29 + (i >> 2) * 17) % 25);
    const uint32_t color = strip.Color(clamp8(currentR * variance / 100),
                                       clamp8(currentG * variance / 100),
                                       clamp8(currentB * variance / 100));
    strip.setPixelColor(i, strip.gamma32(color));
  }
  strip.show();
}

void chooseNewSceneMode() {
  const SceneMode previous = sceneMode;
  do { sceneMode = static_cast<SceneMode>(random(0, 3)); } while (sceneMode == previous);
  sceneModePeriodMs = (uint32_t)random(7, 16) * 60000UL;
  lastSceneModeChangeMs = millis();
}

void updateOutputs() {
  bool scheduled = isAutomaticWindow();
  if (manualMode == ManualMode::BothOff) scheduled = false;
  if (manualMode == ManualMode::BothOn) {
    tvActive = true;
    lampActive = true;
  } else if (!scheduled) {
    tvActive = false;
    lampActive = false;
  } else {
    if ((uint32_t)(millis() - lastSceneModeChangeMs) >= sceneModePeriodMs) chooseNewSceneMode();
    tvActive = sceneMode == SceneMode::TVOnly || sceneMode == SceneMode::Both;
    lampActive = sceneMode == SceneMode::LampOnly || sceneMode == SceneMode::Both;
  }
  digitalWrite(LAMP_DRIVER_PIN, lampActive ? HIGH : LOW);
  static bool previouslyOn = false;
  if (tvActive) {
    if (!previouslyOn) nextSceneColors();
    renderTV();
  } else if (previouslyOn) {
    strip.clear(); strip.show();
    currentR = currentG = currentB = 0;
  }
  previouslyOn = tvActive;
}

String modeName() {
  if (manualMode == ManualMode::BothOn) return "Włączone ręcznie";
  if (manualMode == ManualMode::BothOff) return "Wyłączone ręcznie";
  return "Harmonogram automatyczny";
}

String formattedSunset() {
  if (!cachedSunsetUtc) return "brak czasu / obliczeń";
  tm sunsetLocal = {};
  localtime_r(&cachedSunsetUtc, &sunsetLocal);
  char stamp[24];
  strftime(stamp, sizeof(stamp), "%H:%M", &sunsetLocal);
  return String(stamp);
}

String renderPage() {
  tm nowLocal = {};
  time_t epoch = 0;
  char timeStamp[40] = "oczekiwanie na NTP";
  if (getClock(nowLocal, epoch)) strftime(timeStamp, sizeof(timeStamp), "%Y-%m-%d %H:%M:%S", &nowLocal);
  String page;
  page.reserve(6200);
  page += F("<!doctype html><html lang='pl'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>PresenceSimulator ESP32-C3</title><style>body{font:16px system-ui,Arial;max-width:670px;margin:22px auto;padding:0 16px;color:#273142;background:#f4f7fb}section{background:white;border-radius:12px;padding:18px;margin:14px 0;box-shadow:0 2px 12px #0001}h1{font-size:1.4em}h2{font-size:1.12em}label{display:block;margin:12px 0 5px}input{width:100%;box-sizing:border-box;padding:10px;border:1px solid #bbb;border-radius:6px}button{padding:10px 16px;border:0;border-radius:7px;background:#174e8d;color:white;margin:4px;cursor:pointer}.off{background:#666}.ok{color:#137844}small{color:#596677}</style></head><body>");
  page += F("<h1>PresenceSimulator · ESP32-C3</h1><section><h2>Status</h2><p>Tryb: <b>");
  page += modeName();
  page += F("</b><br>FakeTV: "); page += tvActive ? "ON" : "OFF";
  page += F(" · Lamp: "); page += lampActive ? "ON" : "OFF";
  page += F("<br>Godzina: "); page += timeStamp;
  page += F("<br>Zachód słońca: "); page += formattedSunset();
  page += F("<br>Adres: "); page += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  page += F("</p><form method='post' action='/mode'><button name='mode' value='auto'>AUTO</button><button name='mode' value='on'>WŁĄCZ</button><button class='off' name='mode' value='off'>WYŁĄCZ</button></form></section>");
  page += F("<section><h2>Ustawienia</h2><form method='post' action='/save'>");
  page += F("<label>SSID Wi-Fi 2,4 GHz</label><input name='ssid' maxlength='32' value='"); page += htmlEscape(settings.ssid); page += F("'>");
  page += F("<label>Hasło Wi-Fi (puste = bez zmian)</label><input type='password' name='password' maxlength='63' autocomplete='new-password' placeholder='••••••••'>");
  page += F("<label>Szerokość geograficzna (-90..90)</label><input name='lat' inputmode='decimal' value='"); page += String(settings.latitude, 6); page += F("'>");
  page += F("<label>Długość geograficzna (-180..180)</label><input name='lon' inputmode='decimal' value='"); page += String(settings.longitude, 6); page += F("'>");
  page += F("<label>Strefa czasowa (POSIX TZ)</label><input name='tz' maxlength='90' value='"); page += htmlEscape(settings.timezone); page += F("'>");
  page += F("<label>Liczba diod (1..120)</label><input type='number' min='1' max='120' name='leds' value='"); page += String(settings.pixelCount); page += F("'>");
  page += F("<label>Jasność (1..255)</label><input type='number' min='1' max='255' name='brightness' value='"); page += String(settings.brightness); page += F("'>");
  page += F("<label>Ile minut przed zachodem rozpocząć (0..180)</label><input type='number' min='0' max='180' name='lead' value='"); page += String(settings.sunsetLeadMinutes); page += F("'>");
  page += F("<label>Godzina zakończenia (1..23)</label><input type='number' min='1' max='23' name='stop' value='"); page += String(settings.stopHour); page += F("'>");
  page += F("<p><button type='submit'>Zapisz i uruchom ponownie</button></p></form></section><small>Źródło koncepcji: Andreas Spiess / SensorsIot/PresenceSimulator (MIT). Panel przeznaczony wyłącznie do zaufanej sieci lokalnej.</small></body></html>");
  return page;
}

void handleSave() {
  double lat = 0, lon = 0;
  if (!parseNumber(server.arg("lat"), lat) || !parseNumber(server.arg("lon"), lon) ||
      lat < -90 || lat > 90 || lon < -180 || lon > 180 ||
      server.arg("ssid").length() > 32 || server.arg("password").length() > 63 ||
      server.arg("tz").length() < 1 || server.arg("tz").length() > 90 ||
      server.arg("leds").toInt() < 1 || server.arg("leds").toInt() > MAX_PIXELS ||
      server.arg("brightness").toInt() < 1 || server.arg("brightness").toInt() > 255 ||
      server.arg("lead").toInt() < 0 || server.arg("lead").toInt() > 180 ||
      server.arg("stop").toInt() < 1 || server.arg("stop").toInt() > 23) {
    server.send(400, "text/plain; charset=utf-8", "Niepoprawne ustawienia.");
    return;
  }
  prefs.putString("ssid", server.arg("ssid"));
  if (server.arg("password").length()) prefs.putString("password", server.arg("password"));
  prefs.putDouble("lat", lat);
  prefs.putDouble("lon", lon);
  prefs.putString("timezone", server.arg("tz"));
  prefs.putUShort("leds", (uint16_t)server.arg("leds").toInt());
  prefs.putUChar("bright", (uint8_t)server.arg("brightness").toInt());
  prefs.putUChar("lead", (uint8_t)server.arg("lead").toInt());
  prefs.putUChar("stop", (uint8_t)server.arg("stop").toInt());
  server.send(200, "text/html; charset=utf-8", "<meta charset='utf-8'>Zapisano. Urządzenie uruchomi się ponownie.");
  delay(400);
  ESP.restart();
}

void startAccessPoint() {
  if (accessPointMode) return;
  WiFi.mode(WIFI_AP_STA);
  accessPointMode = WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("[AP] %s | IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void startWiFi(bool forceSetup) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  if (!forceSetup && settings.ssid.length()) {
    WiFi.begin(settings.ssid.c_str(), settings.password.c_str());
    Serial.print("Łączenie z Wi-Fi");
    const uint32_t beginMs = millis();
    while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - beginMs) < 15000) {
      delay(250); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\n[WiFi] %s\n", WiFi.localIP().toString().c_str());
      return;
    }
    Serial.println("\nBrak Wi-Fi. Włączam AP konfiguracyjny.");
  }
  startAccessPoint();
}

void setup() {
  Serial.begin(115200);
  delay(250);
  pinMode(SETUP_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LAMP_DRIVER_PIN, OUTPUT);
  digitalWrite(LAMP_DRIVER_PIN, LOW);
  const bool forcedSetup = digitalRead(SETUP_BUTTON_PIN) == LOW;
  loadSettings();
  strip.begin();
  applyLedSettings();
  randomSeed(esp_random());
  sceneModePeriodMs = (uint32_t)random(7, 16) * 60000UL;
  nextSceneColors();

  startWiFi(forcedSetup);
  configTzTime(settings.timezone.c_str(), "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  if (WiFi.status() == WL_CONNECTED && MDNS.begin(MDNS_NAME)) {
    Serial.println("http://presence-c3.local/");
  }

  server.on("/", HTTP_GET, []() { server.send(200, "text/html; charset=utf-8", renderPage()); });
  server.on("/mode", HTTP_POST, []() {
    if (server.arg("mode") == "on") manualMode = ManualMode::BothOn;
    else if (server.arg("mode") == "off") manualMode = ManualMode::BothOff;
    else if (server.arg("mode") == "auto") manualMode = ManualMode::Auto;
    else { server.send(400, "text/plain", "Unknown mode"); return; }
    server.sendHeader("Location", "/"); server.send(303, "text/plain", "");
  });
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() { server.send(404, "text/plain", "Not Found"); });
  server.begin();
  Serial.println("PresenceSimulator ESP32-C3 ready.");
}

void loop() {
  server.handleClient();
  updateOutputs();
  // If station loses Wi-Fi for a while, expose configuration AP again.
  if (WiFi.status() != WL_CONNECTED && settings.ssid.length()) {
    if (connectionLostSinceMs == 0) connectionLostSinceMs = millis();
    if ((uint32_t)(millis() - lastWiFiAttemptMs) > 30000) {
      lastWiFiAttemptMs = millis();
      WiFi.reconnect();
    }
    if (!accessPointMode && (uint32_t)(millis() - connectionLostSinceMs) > 30000)
      startAccessPoint();
  } else {
    connectionLostSinceMs = 0;
  }
  delay(2);
}