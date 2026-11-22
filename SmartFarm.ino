#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <DHT.h>

const char* WIFI_SSID = "2.4G_thanaphong";
const char* WIFI_PASS = "0952568430";
const char* API_KEY = "q1gg5adEcYs6Yg_GhqEUyoFuIp6BMUHMqvCv7MGpW7A";

#define SOIL_PIN   34
#define DHT_PIN    4
#define DHT_TYPE   DHT22
#define FLOAT_PIN  27
#define RELAY_PIN  26

#define RELAY_ON  LOW
#define RELAY_OFF HIGH

const int SOIL_DRY = 3200;
const int SOIL_WET = 1300;

const unsigned long MAX_PUMP_RUN_MS = 60000UL;
const unsigned long SENSOR_INTERVAL_MS = 2000UL;

DHT dht(DHT_PIN, DHT_TYPE);
WebServer server(80);

bool pumpOn = false;
bool autoMode = true;
bool waterAvailable = false;
bool pumpTimeoutLock = false;
int threshold = 35;
int soilPercent = 0;
int soilRaw = 0;
float temperatureC = NAN;
float humidityPct = NAN;
unsigned long pumpStartedAt = 0;
unsigned long lastSensorRead = 0;
String lastStopReason = "เริ่มต้นระบบ";

bool readWaterAvailable() {
  return digitalRead(FLOAT_PIN) == LOW;
}

int readSoilPercent() {
  soilRaw = analogRead(SOIL_PIN);
  long pct = map(soilRaw, SOIL_DRY, SOIL_WET, 0, 100);
  return constrain((int)pct, 0, 100);
}

void stopPump(const String& reason) {
  digitalWrite(RELAY_PIN, RELAY_OFF);
  if (pumpOn) Serial.println("Pump OFF: " + reason);
  pumpOn = false;
  lastStopReason = reason;
}

bool startPump() {
  if (pumpTimeoutLock) {
    stopPump("ล็อกปั๊มหลังครบเวลาสูงสุด รอความชื้นถึงเกณฑ์");
    return false;
  }
  if (!waterAvailable) {
    stopPump("ไม่มีน้ำในถัง");
    return false;
  }
  if (!pumpOn) {
    digitalWrite(RELAY_PIN, RELAY_ON);
    pumpOn = true;
    pumpStartedAt = millis();
    Serial.println("Pump ON");
  }
  return true;
}

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

bool authorizedRequest() {
  if (!server.hasHeader("X-API-Key")) return false;
  return server.header("X-API-Key") == API_KEY;
}

bool requireApiKey() {
  if (authorizedRequest()) return true;
  addCorsHeaders();
  server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}


void sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  addCorsHeaders();
  server.send(code, "application/json; charset=utf-8", out);
}

bool parseBody(JsonDocument& doc) {
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  return !err;
}

void handleStatus() {
  if (!requireApiKey()) return;
  StaticJsonDocument<384> doc;
  doc["soil"] = soilPercent;
  doc["soilRaw"] = soilRaw;
  doc["temperature"] = isnan(temperatureC) ? -999 : temperatureC;
  doc["humidity"] = isnan(humidityPct) ? -999 : humidityPct;
  doc["water"] = waterAvailable;
  doc["pump"] = pumpOn;
  doc["mode"] = autoMode ? "auto" : "manual";
  doc["threshold"] = threshold;
  doc["ip"] = WiFi.localIP().toString();
  doc["reason"] = lastStopReason;
  sendJson(200, doc);
}

void handlePump() {
  if (!requireApiKey()) return;
  StaticJsonDocument<128> doc;
  if (!parseBody(doc) || !doc["on"].is<bool>()) {
    StaticJsonDocument<128> err;
    err["ok"] = false; err["error"] = "ส่ง JSON {\"on\":true/false}";
    sendJson(400, err); return;
  }
  if (autoMode) {
    StaticJsonDocument<128> err;
    err["ok"] = false; err["error"] = "กรุณาเปลี่ยนเป็นโหมด manual ก่อน";
    sendJson(409, err); return;
  }
  bool on = doc["on"].as<bool>();
  if (on && !waterAvailable) {
    StaticJsonDocument<128> err;
    err["ok"] = false; err["error"] = "ไม่สามารถเปิดปั๊มได้: ระดับน้ำต่ำ";
    sendJson(409, err); return;
  }
  if (on) startPump(); else stopPump("สั่งปิดจากเว็บไซต์");
  StaticJsonDocument<128> res; res["ok"] = true; res["pump"] = pumpOn;
  sendJson(200, res);
}

void handleThreshold() {
  if (!requireApiKey()) return;
  StaticJsonDocument<128> doc;
  if (!parseBody(doc) || !doc["value"].is<int>()) {
    StaticJsonDocument<128> err; err["ok"] = false; err["error"] = "ค่า threshold ไม่ถูกต้อง";
    sendJson(400, err); return;
  }
  threshold = constrain(doc["value"].as<int>(), 10, 80);
  StaticJsonDocument<128> res; res["ok"] = true; res["threshold"] = threshold;
  sendJson(200, res);
}

void handleMode() {
  if (!requireApiKey()) return;
  StaticJsonDocument<128> doc;
  if (!parseBody(doc) || !doc["mode"].is<const char*>()) {
    StaticJsonDocument<128> err; err["ok"] = false; err["error"] = "mode ต้องเป็น auto หรือ manual";
    sendJson(400, err); return;
  }
  String mode = doc["mode"].as<String>();
  if (mode == "auto") {
    autoMode = true;
  } else if (mode == "manual") {
    autoMode = false;
    stopPump("เปลี่ยนเป็น Manual");
  } else {
    StaticJsonDocument<128> err; err["ok"] = false; err["error"] = "mode ไม่ถูกต้อง";
    sendJson(400, err); return;
  }
  StaticJsonDocument<128> res; res["ok"] = true; res["mode"] = autoMode ? "auto" : "manual";
  sendJson(200, res);
}

void handleOptions() {
  addCorsHeaders();
  server.send(204);
}

void updateSensors() {
  unsigned long now = millis();
  if (now - lastSensorRead < SENSOR_INTERVAL_MS) return;
  lastSensorRead = now;

  soilPercent = readSoilPercent();
  waterAvailable = readWaterAvailable();
  Serial.print("Soil ADC: "); Serial.print(soilRaw);
  Serial.print(" | Soil moisture: "); Serial.print(soilPercent); Serial.println("%");

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  temperatureC = t;
  humidityPct = h;
}

void controlPump() {
  if (!waterAvailable) {
    stopPump("ระดับน้ำต่ำ - ระบบป้องกันปั๊ม");
    return;
  }

  if (pumpOn && millis() - pumpStartedAt >= MAX_PUMP_RUN_MS) {
    pumpTimeoutLock = true;
    stopPump("ครบเวลาทำงานสูงสุด 60 วินาที - ล็อกจนกว่าความชื้นถึงเกณฑ์");
    return;
  }

  if (pumpTimeoutLock) {
    if (soilPercent >= threshold) pumpTimeoutLock = false;
    else return;
  }

  if (autoMode) {
    if (soilPercent < threshold) startPump();
    else stopPump("ความชื้นถึงเกณฑ์แล้ว");
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);
  pinMode(FLOAT_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  dht.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-Fi connected");
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/pump", HTTP_POST, handlePump);
  server.on("/api/threshold", HTTP_POST, handleThreshold);
  server.on("/api/mode", HTTP_POST, handleMode);
  server.on("/api/status", HTTP_OPTIONS, handleOptions);
  server.on("/api/pump", HTTP_OPTIONS, handleOptions);
  server.on("/api/threshold", HTTP_OPTIONS, handleOptions);
  server.on("/api/mode", HTTP_OPTIONS, handleOptions);
  server.onNotFound([]() { addCorsHeaders(); server.send(404, "application/json", "{\"error\":\"not found\"}"); });
  const char* headerKeys[] = {"X-API-Key"};
  server.collectHeaders(headerKeys, 1);
  server.begin();
  Serial.println("Smart Farm API started");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    stopPump("Wi-Fi หลุด");
    delay(10);
    return;
  }
  server.handleClient();
  updateSensors();
  controlPump();
  delay(2);
}
