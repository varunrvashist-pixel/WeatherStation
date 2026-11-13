#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_VCNL4200.h>   // NEW: VCNL4200 on-site fog sensor
#include <ArduinoOTA.h>
#include <time.h>            // NEW: local time, so the Rain column can reset each day

// ---- WiFi Configuration ----
const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

// ---- Google Apps Script Web App URL ----
const char* scriptURL = "YOUR_APPS_SCRIPT_WEB_APP_URL";

// ---- OTA Update Settings ----
const char* otaHostname = "DIYweatherstation";
const char* otaPassword = "YOUR_OTA_PASSWORD";

Adafruit_BME280 bme;          // primary, shielded — SDO floating — 0x76
Adafruit_BME280 bme2;         // TEMP: unshielded, radiation-shield A/B test — SDO tied to 3.3V — 0x77
bool bme2Present = false;     // tracks whether the second sensor init succeeded, so this
                               // temporary bench-test sensor never halts the station if
                               // it's unplugged or removed once the A/B test is done
Adafruit_VCNL4200 vcnl4200;   // NEW
bool vcnl4200Present = false; // NEW: tracks whether init succeeded, so a wiring
                               //      issue on this add-on sensor never halts
                               //      the rest of the station (unlike BME280,
                               //      which is treated as core/required below)

// ---- NEW: Fog Detection (VCNL4200 proximity + BME280 humidity) ----
// Algorithm from the write-up's Section 6, adapted from the published field
// study: EWMA baseline + humidity gate + 2-minute persistence requirement.
//
// *** PROX_MARGIN is a PLACEHOLDER, not a trustworthy threshold. ***
// Raw VCNL4200 proximity counts depend on this specific sensor's LED
// current/integration-time settings, its mounting distance from open air,
// and local IR-reflective surroundings — there's no datasheet number that
// safely substitutes for real bench/field data on YOUR unit. Before relying
// on this at all: log proxRaw for a few full days of clear weather, look at
// how much it naturally wanders (that's your noise floor), then set
// PROX_MARGIN comfortably above that noise floor. Re-check it the first
// time real fog or heavy mist actually occurs and compare the logged jump
// against the margin you picked.
const float FOG_EWMA_ALPHA     = 0.05;    // baseline smoothing (slow-moving)
const uint16_t PROX_MARGIN     = 10;      // counts above baseline; paper used 5 (built-in LED) — tune from real data
const float FOG_HUMIDITY_GATE  = 85.0;    // %RH, from the write-up
const unsigned long FOG_PERSIST_MS = 120000; // 2 minutes, from the write-up

bool fogBaselineInitialized = false;
float fogProxBaseline = 0;
unsigned long fogElevatedStartTime = 0; // 0 = not currently elevated
bool fogDetected = false;

// ---- Timing Intervals ----
const unsigned long sendInterval  = 60000; // Cloud upload every 60s
const unsigned long printInterval = 3000;  // Serial monitor update every 3s
unsigned long lastSendTime  = 0;
unsigned long lastPrintTime = 0;

// ---- Anemometer (Wind Speed) Setup ----
const int anemometerPin = D2; // Using D2 alias for Nano ESP32
volatile unsigned long totalWindClicks = 0;
volatile unsigned long intervalWindClicks = 0;
volatile unsigned long lastAnemometerTime = 0;
const unsigned long anemometerDebounceMs = 15;

void IRAM_ATTR countAnemometerClick() {
  unsigned long now = millis();
  if (now - lastAnemometerTime > anemometerDebounceMs) {
    totalWindClicks++;
    intervalWindClicks++;
    lastAnemometerTime = now;
  }
}

// ---- Rain Gauge Setup ----
const int rainGaugePin = D4; // Using D4 alias for Nano ESP32
volatile unsigned long rainClicks = 0;
volatile unsigned long lastRainTime = 0;
const unsigned long rainDebounceMs = 200;
const float inchesPerClick = 0.011;

// NEW: "rain today" bookkeeping. rainClicks keeps counting since boot; the value we
// report is the clicks since the start of the current local day (Pacific time).
unsigned long rainClicksAtMidnight = 0;
int lastRainDay = -1;

void IRAM_ATTR countRainClick() {
  unsigned long now = millis();
  if (now - lastRainTime > rainDebounceMs) {
    rainClicks++;
    lastRainTime = now;
  }
}

// ---- Wind Vane Setup ----
const int windVanePin = A0;
const float pulldownResistor = 10000.0;

struct VaneReading {
  float resistance;
  int degrees;
  const char* label;
};

VaneReading vaneTable[] = {
  {33000,   0,   "N"},
  {6570,    23,  "NNE"},
  {8200,    45,  "NE"},
  {891,     68,  "ENE"},
  {1000,    90,  "E"},
  {688,     113, "ESE"},
  {2200,    135, "SE"},
  {1410,    158, "SSE"},
  {3900,    180, "S"},
  {3140,    203, "SSW"},
  {16000,   225, "SW"},
  {14120,   248, "WSW"},
  {120000,  270, "W"},
  {42120,   293, "WNW"},
  {64900,   315, "NW"},
  {21880,   338, "NNW"}
};

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());

    ArduinoOTA.setHostname(otaHostname);
    ArduinoOTA.setPassword(otaPassword);
    ArduinoOTA.begin();
    Serial.println("OTA ready.");

    // NEW: sync the clock (Pacific time, handles daylight saving) for the daily rain reset
    configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println("\nWiFi failed to connect - continuing offline.");
  }

  // Primary (shielded) BME280 is fixed at 0x76 — its SDO pin is left
  // unconnected, and this breakout's internal pull-down makes that the
  // default address (NOT 0x77 — confirmed by board behavior: hardcoding
  // 0x77 here failed to find it). This one is treated as core/required,
  // same as before.
  if (!bme.begin(0x76)) {
    Serial.println("Could not find primary BME280 sensor! Check wiring.");
    while (1) delay(10);
  }
  Serial.println("Primary BME280 initialized (0x76).");

  // TEMP: second (unshielded) BME280 for the radiation-shield A/B comparison,
  // fixed at 0x77 — its SDO pin must be wired to 3.3V (NOT GND) to get this
  // address, since the primary already occupies 0x76 by default. Non-fatal
  // on failure — it's a temporary bench-test addition, not a core station
  // sensor, so the station should keep running normally if it's unplugged
  // once the comparison is done.
  if (bme2.begin(0x77)) {
    bme2Present = true;
    Serial.println("Second BME280 initialized (0x77) — radiation-shield A/B test.");
  } else {
    Serial.println("Second BME280 not found — continuing without A/B comparison sensor.");
  }

  // NEW: VCNL4200 init — non-fatal on failure, since this is an add-on
  // sensor, not one of the core station measurements
  if (vcnl4200.begin()) {
    vcnl4200.setProxShutdown(false);
    vcnl4200.setProxLEDCurrent(VCNL4200_LED_I_200MA);
    vcnl4200.setProxIntegrationTime(VCNL4200_PS_IT_8T);
    vcnl4200.setALSshutdown(false);
    vcnl4200.setALSIntegrationTime(VCNL4200_ALS_IT_100MS);
    vcnl4200Present = true;
    Serial.println("VCNL4200 initialized.");
  } else {
    Serial.println("VCNL4200 not found — continuing without on-site fog sensor.");
  }

  pinMode(anemometerPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(anemometerPin), countAnemometerClick, FALLING);

  pinMode(rainGaugePin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(rainGaugePin), countRainClick, FALLING);
}

// Multi-sample ADC to smooth electrical noise on the wind vane
String readWindDirection(int* degreesOut) {
  long adcSum = 0;
  for (int i = 0; i < 16; i++) {
    adcSum += analogRead(windVanePin);
    delay(2);
  }
  float adcValue = adcSum / 16.0;
  float voltage = (adcValue / 4095.0) * 3.3;

  if (voltage <= 0.05) {
    *degreesOut = -1;
    return "N/A";
  }

  float vaneResistance = pulldownResistor * ((3.3 / voltage) - 1.0);

  int closestIndex = 0;
  float closestDiff = abs(vaneResistance - vaneTable[0].resistance);

  for (int i = 1; i < 16; i++) {
    float diff = abs(vaneResistance - vaneTable[i].resistance);
    if (diff < closestDiff) {
      closestDiff = diff;
      closestIndex = i;
    }
  }

  *degreesOut = vaneTable[closestIndex].degrees;
  return vaneTable[closestIndex].label;
}

// Returns inches of rain so far TODAY (resets just after local midnight).
// A reboot restarts the count from zero, so a mid-day restart under-reports that day.
float getRainInches() {
  noInterrupts();
  unsigned long clicks = rainClicks;
  interrupts();

  struct tm t;
  if (getLocalTime(&t, 0)) {            // false until the clock has synced
    if (lastRainDay == -1) {
      lastRainDay = t.tm_yday;          // first sync: everything counted since boot is today's
    } else if (t.tm_yday != lastRainDay) {
      lastRainDay = t.tm_yday;          // a new day started
      rainClicksAtMidnight = clicks;    // start today from zero
    }
  }
  return (clicks - rainClicksAtMidnight) * inchesPerClick;
}

void loop() {
  ArduinoOTA.handle(); // Serviced on every loop iteration without blocking delays

  unsigned long currentMillis = millis();

  // --- 1. Serial Diagnostics (Every 3 seconds) ---
  if (currentMillis - lastPrintTime >= printInterval) {
    float elapsedSec = (currentMillis - lastPrintTime) / 1000.0;
    lastPrintTime = currentMillis;

    noInterrupts();
    unsigned long instantClicks = intervalWindClicks;
    intervalWindClicks = 0;
    interrupts();

    float instantWindSpeed = (instantClicks / elapsedSec) * 1.492;

    float tempF = bme.readTemperature() * 1.8 + 32.0;
    float humidity = bme.readHumidity();
    float pressure = bme.readPressure() / 100.0F;

    int windDegrees;
    String windDir = readWindDirection(&windDegrees);

    Serial.printf("Temp: %.1f F | Hum: %.1f %% | Pres: %.1f hPa\n", tempF, humidity, pressure);
    Serial.printf("Wind: %.1f mph (%s, %d deg) | Rain: %.3f in\n", instantWindSpeed, windDir.c_str(), windDegrees, getRainInches());

    // TEMP: second BME280 diagnostics (radiation-shield A/B comparison)
    if (bme2Present) {
      float tempF2 = bme2.readTemperature() * 1.8 + 32.0;
      float humidity2 = bme2.readHumidity();
      float pressure2 = bme2.readPressure() / 100.0F;
      Serial.printf("BME2 (unshielded): Temp: %.1f F | Hum: %.1f %% | Pres: %.1f hPa\n", tempF2, humidity2, pressure2);
    }

    // NEW: VCNL4200 diagnostics + fog detection
    if (vcnl4200Present) {
      uint16_t proxRaw = vcnl4200.readProxData();
      uint16_t alsRaw  = vcnl4200.readALSdata();

      // First-ever reading seeds the baseline directly, so we don't start
      // from 0 and immediately read as "wildly elevated"
      if (!fogBaselineInitialized) {
        fogProxBaseline = proxRaw;
        fogBaselineInitialized = true;
      }

      bool proxElevated = (proxRaw > fogProxBaseline + PROX_MARGIN);

      if (proxElevated) {
        if (fogElevatedStartTime == 0) {
          fogElevatedStartTime = currentMillis; // mark when the elevation began
        }
        // Baseline is frozen while elevated, so a real, sustained fog event
        // doesn't slowly get absorbed into "normal" and erase its own signal
      } else {
        fogElevatedStartTime = 0;
        fogProxBaseline += FOG_EWMA_ALPHA * (proxRaw - fogProxBaseline);
      }

      bool humidityGatePass = (humidity >= FOG_HUMIDITY_GATE);
      bool persistencePass = (fogElevatedStartTime != 0) &&
                              (currentMillis - fogElevatedStartTime >= FOG_PERSIST_MS);

      fogDetected = proxElevated && humidityGatePass && persistencePass;

      Serial.printf("Fog sensor: prox=%u counts (baseline=%.0f) | ALS=%u lux | RH gate=%s | FOG=%s\n",
                    proxRaw, fogProxBaseline, alsRaw,
                    humidityGatePass ? "pass" : "fail",
                    fogDetected ? "YES" : "no");
    }

    Serial.println("----------------------------------------");
  }

  // --- 2. Google Sheets Cloud Upload (Every 60 seconds) ---
  if (currentMillis - lastSendTime >= sendInterval) {
    float elapsedSec = (currentMillis - lastSendTime) / 1000.0;
    lastSendTime = currentMillis;

    // Calculate sustained wind speed across the entire 60 seconds
    noInterrupts();
    unsigned long periodClicks = totalWindClicks;
    totalWindClicks = 0;
    interrupts();

    float sustainedWindSpeed = (periodClicks / elapsedSec) * 1.492;

    float tempF = bme.readTemperature() * 1.8 + 32.0;
    float humidity = bme.readHumidity();
    float pressure = bme.readPressure() / 100.0F;

    int windDegrees;
    String windDir = readWindDirection(&windDegrees);
    float totalRain = getRainInches();

    // TEMP: second BME280 reading for the radiation-shield A/B comparison.
    // Defaults (0.0) are sent if the sensor isn't present, so the upload
    // payload always has a consistent shape; the sheet column will just
    // read 0.0 for that row rather than being missing.
    float tempF2 = 0.0;
    float humidity2 = 0.0;
    float pressure2 = 0.0;
    if (bme2Present) {
      tempF2 = bme2.readTemperature() * 1.8 + 32.0;
      humidity2 = bme2.readHumidity();
      pressure2 = bme2.readPressure() / 100.0F;
    }

    // NEW: grab a fresh VCNL4200 reading right at upload time (spot reading,
    // not an average across the 60s window — fine for this initial bench/
    // deployment phase; the EWMA-baseline fog-detection logic described in
    // the write-up is a separate, later addition on top of this raw data)
    uint16_t proxRaw = 0;
    uint16_t alsRaw = 0;
    if (vcnl4200Present) {
      proxRaw = vcnl4200.readProxData();
      alsRaw  = vcnl4200.readALSdata();
    }

    if (WiFi.status() == WL_CONNECTED) {
      // NEW: fogDetected reflects the most recent 3-second evaluation above,
      // not a fresh check at upload time — that's fine, since the detection
      // state changes on a 2-minute timescale, much slower than this 60s cadence
      sendToGoogleSheets(tempF, humidity, pressure, sustainedWindSpeed, windDir, windDegrees, totalRain, proxRaw, alsRaw, fogDetected, tempF2, humidity2, pressure2, bme2Present);
    } else {
      Serial.println("Skipping upload: WiFi disconnected.");
    }
  }
}

void sendToGoogleSheets(float tempF, float humidity, float pressure, float windSpeed, String windDir, int windDegrees, float totalRain, uint16_t proxRaw, uint16_t alsRaw, bool fogFlag, float tempF2, float humidity2, float pressure2, bool bme2Present) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  String url = String(scriptURL) +
               "?temp=" + String(tempF, 1) +
               "&humidity=" + String(humidity, 1) +
               "&pressure=" + String(pressure, 1) +
               "&windspeed=" + String(windSpeed, 1) +
               "&winddir=" + windDir +
               "&winddeg=" + String(windDegrees) +
               "&rain=" + String(totalRain, 3) +
               "&prox=" + String(proxRaw) +      // NEW
               "&als=" + String(alsRaw) +        // NEW
               "&fog=" + String(fogFlag ? 1 : 0) + // NEW
               "&temp2=" + String(tempF2, 1) +           // TEMP: 2nd BME280 (A/B test)
               "&humidity2=" + String(humidity2, 1) +    // TEMP: 2nd BME280 (A/B test)
               "&pressure2=" + String(pressure2, 1) +    // TEMP: 2nd BME280 (A/B test)
               "&bme2=" + String(bme2Present ? 1 : 0);   // TEMP: flags whether bme2 was actually present this reading

  Serial.println("Uploading 60s sustained data to Google Sheets...");

  if (https.begin(client, url)) {
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int httpCode = https.GET();

    if (httpCode > 0) {
      Serial.printf("Upload successful (HTTP %d)\n", httpCode);
    } else {
      Serial.printf("Upload failed: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
  } else {
    Serial.println("Failed to connect to Google Apps Script URL.");
  }
}
