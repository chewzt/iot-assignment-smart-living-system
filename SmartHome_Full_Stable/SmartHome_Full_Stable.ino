
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <time.h>

// =====================================================
// SMART HOME LIVING SYSTEM
// Firebase-integrated version for the remote control panel
// Logic preserved:
// 1. Smoke has highest priority
// 2. No smoke -> temperature controls fan
// 3. No smoke -> light controls LED + curtain
// 4. Manual mode overrides auto for each device
// =====================================================

// --------------------------
// Firebase / Home
// --------------------------
const char* FIREBASE_DB_URL = "https://iot-smart-living-system-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* DATABASE_AUTH_TOKEN = "";   // keep empty only if RTDB rules are open
const char* HOME_ID = "home_001";

// --------------------------
// Pins
// --------------------------
const int PIN_MQ2 = 34;
const int PIN_LDR = 35;
const int PIN_DHT = 4;

const int PIN_BUZZER = 25;
const int PIN_LED = 2;
const int PIN_FAN_RELAY = 26;
const int PIN_CURTAIN_SERVO = 13;

// --------------------------
// Hardware settings
// --------------------------
const bool BUZZER_IS_PASSIVE = false;   // false = active buzzer, true = passive buzzer
const bool BUZZER_ACTIVE_LOW = false;
const bool LED_ACTIVE_LOW = false;
const bool RELAY_ACTIVE_LOW = false;

// Curtain servo angles
const int CURTAIN_CLOSED_ANGLE = 0;
const int CURTAIN_OPEN_ANGLE = 65;
const unsigned long CURTAIN_MOVE_DELAY_MS = 1000;

// --------------------------
// DHT
// --------------------------
#define DHTTYPE DHT22
DHT dht(PIN_DHT, DHTTYPE);
Servo myServo;

// --------------------------
// Default thresholds (can be overwritten by Firebase)
// --------------------------
int SMOKE_TRIGGER_THRESHOLD = 2000;
int SMOKE_CLEAR_THRESHOLD = 1200;   // hysteresis clear
float TEMP_HIGH_THRESHOLD = 30.0;
int LDR_BRIGHT_THRESHOLD = 800;
int LDR_DARK_THRESHOLD = 2000;

// --------------------------
// Timing
// --------------------------
const unsigned long SENSOR_INTERVAL_MS = 300;
const unsigned long DHT_INTERVAL_MS = 2000;
const unsigned long SERIAL_INTERVAL_MS = 2000;
const unsigned long FIREBASE_POLL_INTERVAL_MS = 2000;
const unsigned long FIREBASE_SYNC_INTERVAL_MS = 3000;
const unsigned long HISTORY_INTERVAL_MS = 5000;

// Buzzer interval pattern
const unsigned long BUZZER_ON_MS = 50;
const unsigned long BUZZER_OFF_MS = 50;

// --------------------------
// Runtime state
// --------------------------
int mq2Value = 0;
int ldrValue = 0;
float temperature = 0.0;
float humidity = 0.0;

bool smokeDetected = false;
bool buzzerPulseState = false;
bool tempHigh = false;
bool isDark = false;
bool isBright = false;

String curtainState = "closed";   // open / closed
String homeMode = "disarmed";

unsigned long lastSensorReadMs = 0;
unsigned long lastDhtReadMs = 0;
unsigned long lastSerialPrintMs = 0;
unsigned long lastFirebasePollMs = 0;
unsigned long lastFirebaseSyncMs = 0;
unsigned long lastHistoryMs = 0;
unsigned long lastBuzzerToggleMs = 0;

bool dirtySync = true;

// device modes / targets from dashboard
String buzzerMode = "manual";
String ledMode = "manual";
String fanMode = "manual";
String curtainMode = "manual";

String buzzerTarget = "off";
String ledTarget = "off";
String fanTarget = "off";
String curtainTarget = "closed";
int curtainTargetPosition = 0;

// previous states for event logging
bool prevSmokeDetected = false;
String prevBuzzerState = "off";
String prevLedState = "off";
String prevFanState = "off";
String prevCurtainState = "closed";

// actual actuator state strings
String buzzerState = "off";
String ledState = "off";
String fanState = "off";

// =====================================================
// Helpers
// =====================================================
String basePath() {
  return String("/homes/") + HOME_ID;
}

void markDirty() {
  dirtySync = true;
}

unsigned long nowMs() {
  time_t now;
  time(&now);
  if (now > 100000) {
    return (unsigned long)now * 1000UL;
  }
  return millis();
}

void writeOutput(int pin, bool on, bool activeLow) {
  digitalWrite(pin, activeLow ? !on : on);
}

String classifySmoke(int value) {
  if (value < 400) return "LOW";
  if (value < SMOKE_TRIGGER_THRESHOLD) return "MEDIUM";
  return "HIGH";
}

String classifyLight(int value) {
  if (value <= LDR_BRIGHT_THRESHOLD) return "BRIGHT";
  if (value >= LDR_DARK_THRESHOLD) return "DARK";
  return "NORMAL";
}

// =====================================================
// Wi-Fi / Time
// =====================================================
void configModeCallback(WiFiManager *wm) {
  Serial.println("Entered WiFi config mode");
  Serial.print("Portal SSID: ");
  Serial.println(wm->getConfigPortalSSID());
  Serial.print("Portal IP: ");
  Serial.println(WiFi.softAPIP());
}

void connectWiFiWithManager() {
  WiFiManager wm;
  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(180);

  bool connected = wm.autoConnect("SmartHome-Setup", "12345678");
  if (!connected) {
    Serial.println("WiFiManager failed. Restarting...");
    delay(2000);
    ESP.restart();
  }

  Serial.println("Wi-Fi connected successfully.");
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");
  Serial.println(WiFi.RSSI());
}

void initTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 20; i++) {
    time_t now;
    time(&now);
    if (now > 100000) {
      Serial.println("NTP time synchronized.");
      return;
    }
    delay(500);
  }
  Serial.println("NTP not ready. Will fallback to millis timestamps.");
}

// =====================================================
// HTTP / Firebase
// =====================================================
String buildUrl(const String& path) {
  String url = String(FIREBASE_DB_URL) + path + ".json";
  if (strlen(DATABASE_AUTH_TOKEN) > 0) {
    url += "?auth=" + String(DATABASE_AUTH_TOKEN);
  }
  return url;
}

bool httpRequest(const String& method, const String& path, const String& payload, String& response, int& httpCode) {
  if (WiFi.status() != WL_CONNECTED) {
    httpCode = -1;
    response = "WiFi disconnected";
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(8000);

  HTTPClient http;
  String url = buildUrl(path);

  if (!http.begin(client, url)) {
    httpCode = -2;
    response = "HTTP begin failed";
    return false;
  }

  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.useHTTP10(true);
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");

  if (method == "GET") {
    httpCode = http.GET();
  } else if (method == "PATCH") {
    httpCode = http.sendRequest("PATCH", payload);
  } else if (method == "POST") {
    httpCode = http.POST(payload);
  } else if (method == "PUT") {
    httpCode = http.PUT(payload);
  } else {
    httpCode = -3;
    response = "Unsupported method";
    http.end();
    return false;
  }

  if (httpCode > 0) response = http.getString();
  else response = http.errorToString(httpCode);

  http.end();
  return httpCode >= 200 && httpCode < 300;
}

bool fbGet(const String& path, DynamicJsonDocument& doc) {
  String response;
  int code = 0;
  bool ok = httpRequest("GET", path, "", response, code);
  if (!ok) {
    Serial.print("GET failed: ");
    Serial.print(code);
    Serial.print(" | ");
    Serial.println(response);
    return false;
  }

  DeserializationError err = deserializeJson(doc, response);
  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }
  return true;
}

bool fbPatch(const String& path, const String& json) {
  String response;
  int code = 0;
  bool ok = httpRequest("PATCH", path, json, response, code);
  if (!ok) {
    Serial.print("PATCH failed: ");
    Serial.print(code);
    Serial.print(" | ");
    Serial.println(response);
  }
  return ok;
}

bool fbPost(const String& path, const String& json) {
  String response;
  int code = 0;
  bool ok = httpRequest("POST", path, json, response, code);
  if (!ok) {
    Serial.print("POST failed: ");
    Serial.print(code);
    Serial.print(" | ");
    Serial.println(response);
  }
  return ok;
}

// =====================================================
// Actuators
// =====================================================
void setBuzzer(bool on) {
  String newState = on ? "on" : "off";
  if (buzzerState == newState) return;

  if (BUZZER_IS_PASSIVE) {
    ledcWriteTone(PIN_BUZZER, on ? 2000 : 0);
  } else {
    writeOutput(PIN_BUZZER, on, BUZZER_ACTIVE_LOW);
  }
  buzzerState = newState;
  markDirty();
}

void setLed(bool on) {
  String newState = on ? "on" : "off";
  if (ledState == newState) return;

  writeOutput(PIN_LED, on, LED_ACTIVE_LOW);
  ledState = newState;
  markDirty();
}

void setFan(bool on) {
  String newState = on ? "on" : "off";
  if (fanState == newState) return;

  writeOutput(PIN_FAN_RELAY, on, RELAY_ACTIVE_LOW);
  fanState = newState;
  markDirty();
}

void moveCurtainTo(int angle, const String& newState) {
  if (curtainState == newState) return;

  Serial.print("Moving curtain to: ");
  Serial.println(newState);

  if (!myServo.attached()) {
    myServo.attach(PIN_CURTAIN_SERVO, 500, 2400);
  }
  myServo.write(angle);
  delay(CURTAIN_MOVE_DELAY_MS);
  myServo.detach();

  curtainState = newState;
  markDirty();

  Serial.println("Curtain arrived and detached.");
}

void setCurtainOpen() {
  moveCurtainTo(CURTAIN_OPEN_ANGLE, "open");
}

void setCurtainClosed() {
  moveCurtainTo(CURTAIN_CLOSED_ANGLE, "closed");
}

void applyManualCommands() {
  if (buzzerMode == "manual") setBuzzer(buzzerTarget == "on");
  if (ledMode == "manual") setLed(ledTarget == "on");
  if (fanMode == "manual") setFan(fanTarget == "on");

  if (curtainMode == "manual") {
    if (curtainTarget == "open") setCurtainOpen();
    else setCurtainClosed();
  }
}

void setSafeDefaults() {
  if (BUZZER_IS_PASSIVE) ledcWriteTone(PIN_BUZZER, 0);
  else writeOutput(PIN_BUZZER, false, BUZZER_ACTIVE_LOW);

  writeOutput(PIN_LED, false, LED_ACTIVE_LOW);
  writeOutput(PIN_FAN_RELAY, false, RELAY_ACTIVE_LOW);

  if (!myServo.attached()) myServo.attach(PIN_CURTAIN_SERVO, 500, 2400);
  myServo.write(CURTAIN_CLOSED_ANGLE);
  delay(CURTAIN_MOVE_DELAY_MS);
  myServo.detach();

  buzzerState = "off";
  ledState = "off";
  fanState = "off";
  curtainState = "closed";
  markDirty();
}

// =====================================================
// Sensors
// =====================================================
void readSmokeAndLdr() {
  mq2Value = analogRead(PIN_MQ2);
  ldrValue = analogRead(PIN_LDR);

  if (!smokeDetected && mq2Value > SMOKE_TRIGGER_THRESHOLD) {
    smokeDetected = true;
    buzzerPulseState = false;
    lastBuzzerToggleMs = millis();
    Serial.println(">>> SMOKE DETECTED");
    markDirty();
  } else if (smokeDetected && mq2Value <= SMOKE_CLEAR_THRESHOLD) {
    smokeDetected = false;
    buzzerPulseState = false;
    setBuzzer(false);
    Serial.println(">>> SMOKE CLEARED");
    markDirty();
  }

  isDark = (ldrValue >= LDR_DARK_THRESHOLD);
  isBright = (ldrValue <= LDR_BRIGHT_THRESHOLD);
}

void readDhtSensor() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity = h;

  tempHigh = (temperature >= TEMP_HIGH_THRESHOLD);
}

// =====================================================
// Logic preserved
// 1. Smoke highest priority
// 2. No smoke -> temperature controls fan
// 3. No smoke -> light controls LED + curtain
// 4. Manual mode overrides auto per device
// =====================================================
void updateBuzzerPattern() {
  if (!smokeDetected) {
    setBuzzer(false);
    return;
  }

  unsigned long now = millis();
  if (buzzerPulseState) {
    if (now - lastBuzzerToggleMs >= BUZZER_ON_MS) {
      buzzerPulseState = false;
      lastBuzzerToggleMs = now;
      setBuzzer(false);
    }
  } else {
    if (now - lastBuzzerToggleMs >= BUZZER_OFF_MS) {
      buzzerPulseState = true;
      lastBuzzerToggleMs = now;
      setBuzzer(true);
    }
  }
}

void applyAutoLogic() {
  // Buzzer
  if (buzzerMode == "auto") {
    if (smokeDetected) updateBuzzerPattern();
    else setBuzzer(false);
  }

  // LED
  if (ledMode == "auto") {
    if (smokeDetected) {
      setLed(true);
    } else if (isDark) {
      setLed(true);
    } else if (isBright) {
      setLed(false);
    }
  }

  // Fan
  if (fanMode == "auto") {
    if (smokeDetected) {
      setFan(true);
    } else if (tempHigh) {
      setFan(true);
    } else {
      setFan(false);
    }
  }

  // Curtain
  if (curtainMode == "auto") {
    if (!smokeDetected) {
      if (isDark) setCurtainClosed();
      else if (isBright || tempHigh) setCurtainOpen();
    }
  }
}

void runLogic() {
  applyManualCommands();
  applyAutoLogic();
}

// =====================================================
// Firebase read / write
// =====================================================
void fetchRemoteState() {
  DynamicJsonDocument doc(8192);
  if (!fbGet(basePath(), doc)) return;

  homeMode = String((const char*)(doc["homeMode"]["state"] | "disarmed"));

  buzzerMode = String((const char*)(doc["devices"]["buzzer"]["mode"] | buzzerMode.c_str()));
  ledMode = String((const char*)(doc["devices"]["ledLight"]["mode"] | ledMode.c_str()));
  fanMode = String((const char*)(doc["devices"]["fanRelay"]["mode"] | fanMode.c_str()));
  curtainMode = String((const char*)(doc["devices"]["curtainServo"]["mode"] | curtainMode.c_str()));

  buzzerTarget = String((const char*)(doc["controls"]["buzzer"]["targetState"] | buzzerTarget.c_str()));
  ledTarget = String((const char*)(doc["controls"]["ledLight"]["targetState"] | ledTarget.c_str()));
  fanTarget = String((const char*)(doc["controls"]["fanRelay"]["targetState"] | fanTarget.c_str()));
  curtainTarget = String((const char*)(doc["controls"]["curtainServo"]["targetState"] | curtainTarget.c_str()));
  curtainTargetPosition = doc["controls"]["curtainServo"]["targetPosition"] | curtainTargetPosition;

  JsonVariant smoke = doc["automation"]["smoke"];
  if (!smoke.isNull()) {
    SMOKE_TRIGGER_THRESHOLD = smoke["mq2Threshold"] | SMOKE_TRIGGER_THRESHOLD;
  }

  JsonVariant temp = doc["automation"]["temperature"];
  if (!temp.isNull()) {
    TEMP_HIGH_THRESHOLD = temp["highThreshold"] | TEMP_HIGH_THRESHOLD;
  }

  JsonVariant light = doc["automation"]["lightIntensity"];
  if (!light.isNull()) {
    LDR_BRIGHT_THRESHOLD = light["brightThreshold"] | LDR_BRIGHT_THRESHOLD;
    LDR_DARK_THRESHOLD = light["darkThreshold"] | LDR_DARK_THRESHOLD;
  }
}

void syncStateToFirebase() {
  DynamicJsonDocument doc(4096);
  unsigned long ts = nowMs();

  JsonObject home = doc.createNestedObject("homeMode");
  home["state"] = homeMode;
  home["updatedAt"] = ts;

  JsonObject status = doc.createNestedObject("status");
  status["smokeDetected"] = smokeDetected;
  status["mq2Value"] = mq2Value;
  status["temperature"] = temperature;
  status["humidity"] = humidity;
  status["lightIntensity"] = ldrValue;
  status["buzzerState"] = buzzerState;
  status["ledState"] = ledState;
  status["fanState"] = fanState;
  status["curtainState"] = curtainState;
  status["lastUpdated"] = ts;

  JsonObject sensors = doc.createNestedObject("sensors");
  JsonObject mq2 = sensors.createNestedObject("mq2");
  mq2["value"] = mq2Value;
  mq2["smokeDetected"] = smokeDetected;
  mq2["updatedAt"] = ts;

  JsonObject dht22 = sensors.createNestedObject("dht22");
  dht22["temperature"] = temperature;
  dht22["humidity"] = humidity;
  dht22["updatedAt"] = ts;

  JsonObject ldr = sensors.createNestedObject("ldr");
  ldr["value"] = ldrValue;
  ldr["updatedAt"] = ts;

  JsonObject devices = doc.createNestedObject("devices");
  JsonObject buzzer = devices.createNestedObject("buzzer");
  buzzer["mode"] = buzzerMode;
  buzzer["state"] = buzzerState;
  buzzer["updatedAt"] = ts;

  JsonObject led = devices.createNestedObject("ledLight");
  led["mode"] = ledMode;
  led["state"] = ledState;
  led["updatedAt"] = ts;

  JsonObject fan = devices.createNestedObject("fanRelay");
  fan["mode"] = fanMode;
  fan["state"] = fanState;
  fan["updatedAt"] = ts;

  JsonObject curtain = devices.createNestedObject("curtainServo");
  curtain["mode"] = curtainMode;
  curtain["state"] = curtainState;
  curtain["position"] = (curtainState == "open") ? 100 : 0;
  curtain["updatedAt"] = ts;

  String payload;
  serializeJson(doc, payload);

  if (fbPatch(basePath(), payload)) {
    dirtySync = false;
  }
}

void pushHistorySnapshot() {
  DynamicJsonDocument doc(1024);
  String key = String(nowMs());

  JsonObject tempObj = doc["temperature"].to<JsonObject>();
  tempObj[key] = temperature;

  JsonObject humObj = doc["humidity"].to<JsonObject>();
  humObj[key] = humidity;

  JsonObject lightObj = doc["lightIntensity"].to<JsonObject>();
  lightObj[key] = ldrValue;

  JsonObject mq2Obj = doc["mq2"].to<JsonObject>();
  mq2Obj[key] = mq2Value;

  String payload;
  serializeJson(doc, payload);
  fbPatch(basePath() + "/history", payload);
}

void logEvent(const String& type, const String& message, const String& triggeredBy, const String& value = "") {
  DynamicJsonDocument doc(512);
  doc["type"] = type;
  doc["message"] = message;
  doc["triggeredBy"] = triggeredBy;
  if (value.length() > 0) doc["value"] = value;
  doc["timestamp"] = nowMs();

  String payload;
  serializeJson(doc, payload);
  fbPost(basePath() + "/events", payload);
}

void logStateChanges() {
  if (smokeDetected != prevSmokeDetected) {
    if (smokeDetected) logEvent("smoke_detected", "Smoke detected by MQ2 sensor", "mq2", String(mq2Value));
    else logEvent("smoke_cleared", "Smoke condition cleared", "mq2", String(mq2Value));
    prevSmokeDetected = smokeDetected;
  }

  if (buzzerState != prevBuzzerState) {
    logEvent(buzzerState == "on" ? "buzzer_on" : "buzzer_off", "Buzzer state changed", "system", buzzerState);
    prevBuzzerState = buzzerState;
  }

  if (ledState != prevLedState) {
    logEvent(ledState == "on" ? "led_on" : "led_off", "LED state changed", "system", ledState);
    prevLedState = ledState;
  }

  if (fanState != prevFanState) {
    logEvent(fanState == "on" ? "fan_on" : "fan_off", "Fan relay state changed", "system", fanState);
    prevFanState = fanState;
  }

  if (curtainState != prevCurtainState) {
    logEvent(curtainState == "open" ? "curtain_opened" : "curtain_closed", "Curtain state changed", "system", curtainState);
    prevCurtainState = curtainState;
  }
}

// =====================================================
// Serial
// =====================================================
void printStatus() {
  Serial.println("==================================");
  Serial.println("Firebase-Integrated Smart Home");
  Serial.print("MQ2 Value        : "); Serial.println(mq2Value);
  Serial.print("Smoke Level      : "); Serial.println(classifySmoke(mq2Value));
  Serial.print("Smoke Detected   : "); Serial.println(smokeDetected ? "YES" : "NO");
  Serial.print("LDR Value        : "); Serial.println(ldrValue);
  Serial.print("Light Level      : "); Serial.println(classifyLight(ldrValue));

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("DHT22            : READ FAILED");
  } else {
    Serial.print("Temperature      : "); Serial.print(temperature, 1); Serial.println(" C");
    Serial.print("Humidity         : "); Serial.print(humidity, 1); Serial.println(" %");
  }

  Serial.print("Temp High        : "); Serial.println(tempHigh ? "YES" : "NO");
  Serial.print("Home Mode        : "); Serial.println(homeMode);
  Serial.print("Modes            : ");
  Serial.print("Buzzer="); Serial.print(buzzerMode);
  Serial.print(" LED="); Serial.print(ledMode);
  Serial.print(" Fan="); Serial.print(fanMode);
  Serial.print(" Curtain="); Serial.println(curtainMode);

  Serial.print("States           : ");
  Serial.print("Buzzer="); Serial.print(buzzerState);
  Serial.print(" LED="); Serial.print(ledState);
  Serial.print(" Fan="); Serial.print(fanState);
  Serial.print(" Curtain="); Serial.println(curtainState);

  Serial.print("RSSI             : "); Serial.println(WiFi.RSSI());
  Serial.println("==================================");
  Serial.println();
}

// =====================================================
// Setup / Loop
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("ESP32 Firebase-Integrated Smart Home");

  pinMode(PIN_MQ2, INPUT);
  pinMode(PIN_LDR, INPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_FAN_RELAY, OUTPUT);
  analogReadResolution(12);

  if (BUZZER_IS_PASSIVE) {
    bool ok = ledcAttach(PIN_BUZZER, 2000, 8);
    Serial.print("LEDC attach buzzer: ");
    Serial.println(ok ? "OK" : "FAILED");
  } else {
    pinMode(PIN_BUZZER, OUTPUT);
  }

  dht.begin();

  ESP32PWM::allocateTimer(0);
  myServo.setPeriodHertz(50);
  myServo.attach(PIN_CURTAIN_SERVO, 500, 2400);
  myServo.write(CURTAIN_CLOSED_ANGLE);
  delay(CURTAIN_MOVE_DELAY_MS);
  myServo.detach();
  curtainState = "closed";

  setSafeDefaults();

  connectWiFiWithManager();
  initTime();

  Serial.println("Warming up MQ2...");
  delay(5000);

  fetchRemoteState();
  syncStateToFirebase();
  logEvent("system_start", "Smart home system initialized", "system");
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi lost. Reconnecting...");
    WiFi.reconnect();
    delay(1000);
  }

  if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = now;
    readSmokeAndLdr();
    runLogic();
    logStateChanges();
  }

  if (now - lastDhtReadMs >= DHT_INTERVAL_MS) {
    lastDhtReadMs = now;
    readDhtSensor();
    runLogic();
    logStateChanges();
  }

  if (WiFi.status() == WL_CONNECTED && now - lastFirebasePollMs >= FIREBASE_POLL_INTERVAL_MS) {
    lastFirebasePollMs = now;
    fetchRemoteState();
  }

  if (WiFi.status() == WL_CONNECTED && (dirtySync || (now - lastFirebaseSyncMs >= FIREBASE_SYNC_INTERVAL_MS))) {
    lastFirebaseSyncMs = now;
    syncStateToFirebase();
  }

  if (WiFi.status() == WL_CONNECTED && now - lastHistoryMs >= HISTORY_INTERVAL_MS) {
    lastHistoryMs = now;
    pushHistorySnapshot();
  }

  if (now - lastSerialPrintMs >= SERIAL_INTERVAL_MS) {
    lastSerialPrintMs = now;
    printStatus();
  }
}
