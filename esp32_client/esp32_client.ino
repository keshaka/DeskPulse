/*
  DeskPulse ESP32 Client (ESP32-2432S028)
  ---------------------------------------
  Features:
  - Connects to WiFi
  - Sends HTTP GET to Flask /stats endpoint
  - Parses JSON with ArduinoJson
  - Prints parsed values to Serial Monitor
  - Handles WiFi and HTTP errors gracefully

  Required libraries:
  - ArduinoJson (by Benoit Blanchon)

  Notes:
  - Update WIFI_SSID, WIFI_PASSWORD, and SERVER_URL below.
  - Example SERVER_URL: http://192.168.1.100:5000/stats
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// -------------------- User Configuration --------------------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Use your PC local IP address and Flask port.
const char* SERVER_URL = "http://192.168.1.100:5000/stats";

// Poll every 2 seconds.
const unsigned long POLL_INTERVAL_MS = 2000;

// WiFi connection timeout.
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

// -------------------- Runtime State --------------------
unsigned long lastPollMs = 0;

// -------------------- Helper Functions --------------------
void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startMs = millis();

  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Connection failed (timeout). Will retry in loop.");
  }
}

void printMissingField(const char* fieldName) {
  Serial.print("[JSON] Missing field: ");
  Serial.println(fieldName);
}

void fetchAndPrintStats() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Skipped: WiFi not connected.");
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);

  Serial.print("[HTTP] GET ");
  Serial.println(SERVER_URL);

  if (!http.begin(SERVER_URL)) {
    Serial.println("[HTTP] Failed to initialize request.");
    http.end();
    return;
  }

  int httpCode = http.GET();

  if (httpCode <= 0) {
    Serial.print("[HTTP] Request failed. Error: ");
    Serial.println(http.errorToString(httpCode));
    http.end();
    return;
  }

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("[HTTP] Unexpected response code: ");
    Serial.println(httpCode);
    String errorBody = http.getString();
    Serial.print("[HTTP] Response body: ");
    Serial.println(errorBody);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  // Keep this reasonably sized for ESP32 memory.
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.print("[JSON] Parse failed: ");
    Serial.println(err.c_str());
    return;
  }

  // Expected shape from Flask:
  // {
  //   "status": "ok",
  //   "data": { ... }
  // }

  const char* status = doc["status"] | "unknown";
  JsonObject data = doc["data"].as<JsonObject>();

  if (data.isNull()) {
    Serial.println("[JSON] Invalid payload: 'data' object not found.");
    return;
  }

  Serial.println("---------------- DeskPulse Stats ----------------");
  Serial.print("status: ");
  Serial.println(status);

  // CPU
  if (data["cpu_percent"].isNull()) {
    printMissingField("data.cpu_percent");
  }
  float cpuPercent = data["cpu_percent"] | -1.0f;
  Serial.print("CPU: ");
  Serial.print(cpuPercent, 1);
  Serial.println(" %");

  // RAM
  JsonObject ram = data["ram"].as<JsonObject>();
  if (ram.isNull()) {
    printMissingField("data.ram");
  }
  float ramUsedPercent = ram["used_percent"] | -1.0f;
  Serial.print("RAM: ");
  Serial.print(ramUsedPercent, 1);
  Serial.println(" %");

  // Disk
  JsonObject disk = data["disk"].as<JsonObject>();
  if (disk.isNull()) {
    printMissingField("data.disk");
  }
  float diskUsedPercent = disk["used_percent"] | -1.0f;
  Serial.print("Disk: ");
  Serial.print(diskUsedPercent, 1);
  Serial.println(" %");

  // Network
  JsonObject network = data["network"].as<JsonObject>();
  if (network.isNull()) {
    printMissingField("data.network");
  }
  unsigned long long bytesSent = network["bytes_sent"] | 0ULL;
  unsigned long long bytesReceived = network["bytes_received"] | 0ULL;
  Serial.print("Network bytes sent: ");
  Serial.println(bytesSent);
  Serial.print("Network bytes received: ");
  Serial.println(bytesReceived);

  // GPU (optional)
  JsonObject gpu = data["gpu"].as<JsonObject>();
  if (!gpu.isNull()) {
    bool gpuAvailable = gpu["available"] | false;
    Serial.print("GPU available: ");
    Serial.println(gpuAvailable ? "yes" : "no");

    float gpuUsage = gpu["gpu_usage_percent"] | -1.0f;
    if (gpuUsage >= 0.0f) {
      Serial.print("GPU usage: ");
      Serial.print(gpuUsage, 1);
      Serial.println(" %");
    }
  }

  // Temperatures (optional)
  JsonObject cpuTemp = data["cpu_temp"].as<JsonObject>();
  if (!cpuTemp.isNull()) {
    bool cpuTempAvailable = cpuTemp["available"] | false;
    if (cpuTempAvailable) {
      float tempC = cpuTemp["celsius"] | -999.0f;
      Serial.print("CPU temp: ");
      Serial.print(tempC, 1);
      Serial.println(" C");
    } else {
      Serial.println("CPU temp: unavailable");
    }
  }

  // Media (optional)
  JsonObject media = data["media"].as<JsonObject>();
  if (!media.isNull()) {
    const char* title = media["title"] | "";
    const char* artist = media["artist"] | "";
    const char* playbackState = media["playback_state"] | "unknown";

    Serial.print("Media state: ");
    Serial.println(playbackState);

    if (strlen(title) > 0 || strlen(artist) > 0) {
      Serial.print("Now playing: ");
      Serial.print(title);
      Serial.print(" - ");
      Serial.println(artist);
    }
  }

  Serial.println("-------------------------------------------------");
}

// -------------------- Arduino Entry Points --------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("DeskPulse ESP32 client starting...");

  connectToWiFi();
}

void loop() {
  // Auto-reconnect if disconnected.
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
  }

  unsigned long nowMs = millis();
  if (nowMs - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = nowMs;
    fetchAndPrintStats();
  }

  delay(10);
}
